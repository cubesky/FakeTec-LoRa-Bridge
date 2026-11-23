//! lbridge - client for the FakeTec LoRa serial bridge.
//!
//! * `lbridge term`   transparent terminal over the LoRa link. The local console is put into
//!                    raw mode and every byte is passed through untouched, so a remote Linux
//!                    console works with full colour (ANSI / 256 / true colour) and all control
//!                    keys and escape sequences (Ctrl-C, arrows, vi, htop, ...).
//! * configuration    `info`, `stat`, `cfg`, `get`, `set`, `at`, `pair`, ... use the device's
//!                    in-band control channel (ESC ] L B ; <AT command> BEL), which works at any
//!                    time without leaving data mode and without the "+++" guard times.
use anyhow::{anyhow, bail, Context, Result};
use clap::{Parser, Subcommand};
use crossterm::style::Stylize;
use serialport::{SerialPort, SerialPortType};
use std::io::{self, Read, Write};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const VID: u16 = 0x239A;
const OSC_PFX: &[u8] = b"\x1b]LB;";
const BEL: u8 = 0x07;
const MENU_KEY: u8 = 0x1d; // Ctrl-]

#[derive(Parser)]
#[command(name = "lbridge", version, about = "FakeTec LoRa serial bridge client (terminal + configuration)")]
struct Cli {
    /// Serial port (COM3, /dev/ttyACM0). Auto-detected when only one bridge is plugged in.
    #[arg(short, long, global = true)]
    port: Option<String>,
    #[arg(short, long, global = true, default_value_t = 115200)]
    baud: u32,
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// List bridges that are plugged in
    List,
    /// Transparent terminal over the LoRa link (default). Ctrl-] opens the local menu.
    Term {
        /// Do not show the link status in the terminal window title
        #[arg(long)]
        no_title: bool,
        /// Echo typed characters locally
        #[arg(long)]
        echo: bool,
    },
    /// Device information
    Info,
    /// Link statistics
    Stat,
    /// Show all settings
    Cfg,
    /// Read one setting, e.g. `get freq`
    Get { key: String },
    /// Change one setting, e.g. `set freq 434.5`
    Set { key: String, value: String },
    /// Send a raw AT command, e.g. `at AT+HOPCH=16`
    At { line: Vec<String> },
    /// Start pairing (run it on both ends)
    Pair {
        #[arg(default_value_t = 60)]
        seconds: u32,
    },
    /// Forget the peer
    Unpair,
    /// Reboot the device
    Reboot,
    /// Reboot into the UF2 / serial DFU bootloader
    Dfu,
    /// Live link status
    Monitor,
}

// ------------------------------------------------------------------ control channel
/// Splits the byte stream from the device into terminal data and control responses.
#[derive(Default)]
struct Demux {
    held: usize,
    body: Option<Vec<u8>>,
}

impl Demux {
    fn feed(&mut self, data: &[u8], out: &mut Vec<u8>, rsp: &mut Vec<String>) {
        for &c in data {
            if self.body.is_some() {
                if c == BEL {
                    let b = self.body.take().unwrap();
                    rsp.push(String::from_utf8_lossy(&b).into_owned());
                } else {
                    self.body.as_mut().unwrap().push(c);
                }
                continue;
            }
            if c == OSC_PFX[self.held] {
                self.held += 1;
                if self.held == OSC_PFX.len() {
                    self.held = 0;
                    self.body = Some(Vec::new());
                }
                continue;
            }
            if self.held > 0 {
                out.extend_from_slice(&OSC_PFX[..self.held]);
                self.held = 0;
                if c == 0x1b {
                    self.held = 1;
                    continue;
                }
            }
            out.push(c);
        }
    }
    /// Nothing more arrived: a held ESC was just an ESC.
    fn flush(&mut self, out: &mut Vec<u8>) {
        if self.held > 0 && self.body.is_none() {
            out.extend_from_slice(&OSC_PFX[..self.held]);
            self.held = 0;
        }
    }
}

fn frame(cmd: &str) -> Vec<u8> {
    let mut v = OSC_PFX.to_vec();
    v.extend_from_slice(cmd.as_bytes());
    v.push(BEL);
    v
}

fn is_timeout(e: &io::Error) -> bool {
    matches!(e.kind(), io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock)
}

struct Dev {
    name: String,
    port: Box<dyn SerialPort>,
}

impl Dev {
    fn cmd(&mut self, line: &str) -> Result<String> {
        self.port.write_all(&frame(line))?;
        self.port.flush().ok();
        let mut dm = Demux::default();
        let (mut out, mut rsp) = (Vec::new(), Vec::new());
        let mut buf = [0u8; 1024];
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            match self.port.read(&mut buf) {
                Ok(n) => dm.feed(&buf[..n], &mut out, &mut rsp),
                Err(e) if is_timeout(&e) => {}
                Err(e) => return Err(e.into()),
            }
            if let Some(r) = rsp.pop() {
                return Ok(r.replace("\r\n", "\n").trim().to_string());
            }
        }
        bail!("no answer from the device on {} (firmware older than 1.1.0?)", self.name)
    }
}

fn bridges() -> Result<Vec<(String, String)>> {
    let mut v = Vec::new();
    for p in serialport::available_ports()? {
        if let SerialPortType::UsbPort(u) = &p.port_type {
            if u.vid == VID {
                let desc = format!(
                    "{} {}",
                    u.product.clone().unwrap_or_default(),
                    u.serial_number.clone().unwrap_or_default()
                );
                v.push((p.port_name.clone(), desc.trim().to_string()));
            }
        }
    }
    Ok(v)
}

fn open(cli: &Cli) -> Result<Dev> {
    let name = match &cli.port {
        Some(p) => p.clone(),
        None => {
            let b = bridges()?;
            match b.len() {
                0 => bail!("no bridge found (USB VID 239A). Plug it in or use --port"),
                1 => b[0].0.clone(),
                _ => bail!(
                    "several bridges found, choose one with --port: {}",
                    b.iter().map(|x| x.0.as_str()).collect::<Vec<_>>().join(", ")
                ),
            }
        }
    };
    if cli.baud == 1200 {
        bail!("1200 baud would reboot the device into its bootloader");
    }
    let mut port = serialport::new(&name, cli.baud)
        .timeout(Duration::from_millis(30))
        .open()
        .with_context(|| format!("cannot open {name}"))?;
    port.write_data_terminal_ready(true).ok();
    port.write_request_to_send(true).ok();
    std::thread::sleep(Duration::from_millis(150));
    Ok(Dev { name, port })
}

// ------------------------------------------------------------------ pretty printing
#[derive(Default, Clone)]
struct Status {
    mode: String,
    role: String,
    rssi: i32,
    snr: f32,
    batt: u32,
    usb: bool,
    tx_rate: u32,
    rx_rate: u32,
    peer_batt: u32,
    queue: u32,
    channel: u32,
    freq: f32,
}

fn parse_status(rsp: &str) -> Option<Status> {
    let line = rsp.lines().find_map(|l| l.trim().strip_prefix("+ST="))?;
    let f: Vec<&str> = line.split(',').collect();
    if f.len() < 12 {
        return None;
    }
    Some(Status {
        mode: f[0].into(),
        role: f[1].into(),
        rssi: f[2].parse().ok()?,
        snr: f[3].parse().ok()?,
        batt: f[4].parse().ok()?,
        usb: f[5] == "1",
        tx_rate: f[6].parse().ok()?,
        rx_rate: f[7].parse().ok()?,
        peer_batt: f[8].parse().ok()?,
        queue: f[9].parse().ok()?,
        channel: f[10].parse().ok()?,
        freq: f[11].parse().ok()?,
    })
}

fn mode_styled(mode: &str) -> String {
    match mode {
        "LINK" => mode.green().bold().to_string(),
        "STBY" => mode.cyan().bold().to_string(),
        "NOOP" => mode.red().bold().to_string(),
        "UNPR" => mode.magenta().bold().to_string(),
        _ => mode.yellow().bold().to_string(),
    }
}

fn status_line(s: &Status) -> String {
    let role = match s.role.as_str() {
        "M" => "master",
        "S" => "slave",
        _ => "-",
    };
    let peer = if s.peer_batt <= 100 { format!(" peer {}%", s.peer_batt) } else { String::new() };
    format!(
        "{} {}  {} {:.3} MHz ch {}  {} {} dBm / {:.1} dB  {} {} B/s  {} {} B/s  {} {}  {} {}%{}{}",
        mode_styled(&s.mode),
        role,
        "freq".dark_grey(),
        s.freq,
        s.channel,
        "rssi".dark_grey(),
        s.rssi,
        s.snr,
        "TX".dark_grey(),
        s.tx_rate,
        "RX".dark_grey(),
        s.rx_rate,
        "Q".dark_grey(),
        s.queue,
        "bat".dark_grey(),
        s.batt,
        if s.usb { " (USB)" } else { "" },
        peer
    )
}

/// Colourise a device answer: "+KEY=value" lines, OK / ERROR, "Name: value" lines.
fn pretty(rsp: &str, nl: &str) -> String {
    let mut o = String::new();
    for l in rsp.lines() {
        let l = l.trim_end();
        if l.is_empty() {
            continue;
        }
        let s = if l == "OK" || l.starts_with("OK ") {
            l.green().to_string()
        } else if l.starts_with("ERROR") {
            l.red().bold().to_string()
        } else if let Some(rest) = l.strip_prefix('+') {
            match rest.split_once('=') {
                Some((k, v)) => format!("{}{} = {}", "+".dark_grey(), k.cyan(), v.yellow()),
                None => l.to_string(),
            }
        } else if let Some((k, v)) = l.split_once(": ") {
            format!("{}: {}", k.cyan(), v)
        } else {
            l.to_string()
        };
        o.push_str(&s);
        o.push_str(nl);
    }
    o
}

fn run_cmd(dev: &mut Dev, line: &str) -> Result<()> {
    let rsp = dev.cmd(line)?;
    print!("{}", pretty(&rsp, "\n"));
    if rsp.lines().any(|l| l.starts_with("ERROR")) {
        bail!("device rejected: {line}");
    }
    Ok(())
}

// ------------------------------------------------------------------ console raw mode
struct RawGuard;
impl RawGuard {
    fn new() -> Result<Self> {
        crossterm::terminal::enable_raw_mode()?;
        vt_mode();
        Ok(RawGuard)
    }
}
impl Drop for RawGuard {
    fn drop(&mut self) {
        let _ = crossterm::terminal::disable_raw_mode();
    }
}

/// Windows: ask the console for VT input (arrow keys etc. arrive as escape sequences, like on
/// Linux) and VT output (colours, cursor control). Linux / macOS terminals do this natively.
#[cfg(windows)]
fn vt_mode() {
    use windows_sys::Win32::System::Console::*;
    unsafe {
        let hin = GetStdHandle(STD_INPUT_HANDLE);
        let mut m = 0u32;
        if GetConsoleMode(hin, &mut m) != 0 {
            let m = (m | ENABLE_VIRTUAL_TERMINAL_INPUT)
                & !(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
            SetConsoleMode(hin, m);
        }
        let hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if GetConsoleMode(hout, &mut m) != 0 {
            SetConsoleMode(hout, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
        }
    }
}
#[cfg(not(windows))]
fn vt_mode() {}

fn put(bytes: &[u8]) {
    let mut so = io::stdout().lock();
    if so.write_all(bytes).is_err() {
        // Windows consoles refuse invalid UTF-8
        let _ = so.write_all(String::from_utf8_lossy(bytes).as_bytes());
    }
    let _ = so.flush();
}

// ------------------------------------------------------------------ terminal
enum Input {
    Pass,
    Menu,
    Line(String),
}

fn menu_help() -> String {
    format!(
        "\r\n{} {}uit  {}tatus  {}ommand  {}air  {}nfo  s{}ttings  Ctrl-] send ^]  other: back\r\n",
        "[lbridge]".black().on_cyan(),
        "q".yellow().bold(),
        "s".yellow().bold(),
        "c".yellow().bold(),
        "p".yellow().bold(),
        "i".yellow().bold(),
        "e".yellow().bold()
    )
}

fn term(dev: Dev, no_title: bool, echo: bool) -> Result<()> {
    let name = dev.name.clone();
    let mut reader = dev.port.try_clone().context("cannot clone the serial port")?;
    let writer = Arc::new(Mutex::new(dev.port));
    let (rsp_tx, rsp_rx) = mpsc::channel::<String>();
    let (in_tx, in_rx) = mpsc::channel::<Vec<u8>>();

    eprintln!(
        "{} connected to {} - {} opens the menu",
        "[lbridge]".black().on_cyan(),
        name.clone().bold(),
        "Ctrl-]".yellow().bold()
    );
    let guard = RawGuard::new()?;

    // device -> console
    std::thread::spawn(move || {
        let mut dm = Demux::default();
        let mut buf = [0u8; 4096];
        loop {
            let (mut out, mut rsp) = (Vec::new(), Vec::new());
            match reader.read(&mut buf) {
                Ok(0) => {}
                Ok(n) => dm.feed(&buf[..n], &mut out, &mut rsp),
                Err(e) if is_timeout(&e) => dm.flush(&mut out),
                Err(e) => {
                    let _ = crossterm::terminal::disable_raw_mode();
                    eprintln!("\r\n{} {}", "[lbridge] port closed:".red(), e);
                    std::process::exit(1);
                }
            }
            if !out.is_empty() {
                put(&out);
            }
            for r in rsp {
                let _ = rsp_tx.send(r);
            }
        }
    });

    // keyboard -> main loop
    std::thread::spawn(move || {
        let mut buf = [0u8; 256];
        let mut si = io::stdin();
        loop {
            match si.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if in_tx.send(buf[..n].to_vec()).is_err() {
                        break;
                    }
                }
            }
        }
    });

    let send = |bytes: &[u8]| -> Result<()> {
        let mut w = writer.lock().unwrap();
        w.write_all(bytes)?;
        w.flush().ok();
        Ok(())
    };
    let ask = |line: &str| -> Result<String> {
        while rsp_rx.try_recv().is_ok() {}
        send(&frame(line))?;
        rsp_rx
            .recv_timeout(Duration::from_secs(3))
            .map(|r| r.replace("\r\n", "\n").trim().to_string())
            .map_err(|_| anyhow!("no answer from the device"))
    };
    let show = |line: &str| match ask(line) {
        Ok(r) => put(pretty(&r, "\r\n").as_bytes()),
        Err(e) => put(format!("{}\r\n", e.to_string().red()).as_bytes()),
    };

    let mut state = Input::Pass;
    let mut next_poll = Instant::now();
    loop {
        if !no_title && matches!(state, Input::Pass) && Instant::now() >= next_poll {
            next_poll = Instant::now() + Duration::from_secs(2);
            if let Some(s) = ask("AT+ST").ok().and_then(|r| parse_status(&r)) {
                put(format!(
                    "\x1b]0;lbridge {} | {} {} | RSSI {} SNR {:.1} | TX {} RX {} B/s | bat {}%\x07",
                    name, s.mode, s.role, s.rssi, s.snr, s.tx_rate, s.rx_rate, s.batt
                )
                .as_bytes());
            }
        }
        let data = match in_rx.recv_timeout(Duration::from_millis(200)) {
            Ok(d) => d,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => break,
        };
        let mut pass = Vec::new();
        for &c in &data {
            match &mut state {
                Input::Pass => {
                    if c == MENU_KEY {
                        put(menu_help().as_bytes());
                        state = Input::Menu;
                    } else {
                        pass.push(c);
                    }
                }
                Input::Menu => {
                    state = Input::Pass;
                    match c {
                        b'q' | b'Q' | 3 => {
                            drop(guard);
                            println!();
                            std::process::exit(0);
                        }
                        MENU_KEY => pass.push(MENU_KEY),
                        b's' | b'S' => {
                            match ask("AT+ST").ok().and_then(|r| parse_status(&r)) {
                                Some(s) => put(format!("{}\r\n", status_line(&s)).as_bytes()),
                                None => put(b"no status\r\n"),
                            }
                            show("AT+STAT");
                        }
                        b'i' | b'I' => show("ATI"),
                        b'e' | b'E' => show("AT+CFG"),
                        b'p' | b'P' => show("AT+PAIR"),
                        b'c' | b'C' => {
                            put(format!("{} ", "AT>".cyan().bold()).as_bytes());
                            state = Input::Line(String::new());
                        }
                        _ => put(b"\r\n"),
                    }
                }
                Input::Line(l) => match c {
                    b'\r' | b'\n' => {
                        put(b"\r\n");
                        let line = l.trim().to_string();
                        state = Input::Pass;
                        if !line.is_empty() {
                            let line = normalise(&line);
                            show(&line);
                        }
                    }
                    0x1b | 3 => {
                        put(b"\r\n");
                        state = Input::Pass;
                    }
                    8 | 127 => {
                        if l.pop().is_some() {
                            put(b"\x08 \x08");
                        }
                    }
                    c if c >= 32 => {
                        l.push(c as char);
                        put(&[c]);
                    }
                    _ => {}
                },
            }
        }
        if !pass.is_empty() {
            if echo {
                put(&pass);
            }
            send(&pass)?;
        }
    }
    Ok(())
}

/// "freq 434" / "freq=434" / "AT+FREQ=434" / "freq?" -> AT command
fn normalise(line: &str) -> String {
    let u = line.trim();
    if u.len() >= 2 && u[..2].eq_ignore_ascii_case("AT") {
        return u.to_string();
    }
    match u.split_once(|c: char| c == ' ' || c == '=') {
        Some((k, v)) => format!("AT+{}={}", k.trim().to_uppercase(), v.trim()),
        None => format!("AT+{}", u.to_uppercase()),
    }
}

fn monitor(dev: &mut Dev) -> Result<()> {
    eprintln!("{} Ctrl-C to stop", "[lbridge]".black().on_cyan());
    loop {
        let rsp = dev.cmd("AT+ST")?;
        match parse_status(&rsp) {
            Some(s) => print!("\r\x1b[2K{}", status_line(&s)),
            None => print!("\r\x1b[2K{}", rsp),
        }
        io::stdout().flush().ok();
        std::thread::sleep(Duration::from_secs(1));
    }
}

fn main() {
    let cli = Cli::parse();
    #[cfg(windows)]
    {
        // colours for the one-shot commands too
        use windows_sys::Win32::System::Console::*;
        unsafe {
            let hout = GetStdHandle(STD_OUTPUT_HANDLE);
            let mut m = 0u32;
            if GetConsoleMode(hout, &mut m) != 0 {
                SetConsoleMode(hout, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
    }
    if let Err(e) = run(cli) {
        let _ = crossterm::terminal::disable_raw_mode();
        eprintln!("{} {:#}", "error:".red().bold(), e);
        std::process::exit(1);
    }
}

fn run(cli: Cli) -> Result<()> {
    match &cli.cmd {
        Some(Cmd::List) => {
            let b = bridges()?;
            if b.is_empty() {
                println!("no bridge found");
            }
            for (p, d) in b {
                println!("{}  {}", p.green().bold(), d);
            }
            Ok(())
        }
        None => term(open(&cli)?, false, false),
        Some(Cmd::Term { no_title, echo }) => term(open(&cli)?, *no_title, *echo),
        Some(c) => {
            let mut dev = open(&cli)?;
            match c {
                Cmd::Info => run_cmd(&mut dev, "ATI"),
                Cmd::Stat => {
                    if let Some(s) = parse_status(&dev.cmd("AT+ST")?) {
                        println!("{}", status_line(&s));
                    }
                    run_cmd(&mut dev, "AT+STAT")
                }
                Cmd::Cfg => run_cmd(&mut dev, "AT+CFG"),
                Cmd::Get { key } => run_cmd(&mut dev, &format!("AT+{}?", key.to_uppercase())),
                Cmd::Set { key, value } => run_cmd(&mut dev, &format!("AT+{}={}", key.to_uppercase(), value)),
                Cmd::At { line } => run_cmd(&mut dev, &normalise(&line.join(" "))),
                Cmd::Pair { seconds } => run_cmd(&mut dev, &format!("AT+PAIR={seconds}")),
                Cmd::Unpair => run_cmd(&mut dev, "AT+UNPAIR"),
                Cmd::Reboot => run_cmd(&mut dev, "AT+REBOOT"),
                Cmd::Dfu => run_cmd(&mut dev, "AT+DFU"),
                Cmd::Monitor => monitor(&mut dev),
                Cmd::List | Cmd::Term { .. } => unreachable!(),
            }
        }
    }
}