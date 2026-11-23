'use strict';
'require view';
'require form';
'require fs';
'require poll';
'require ui';

function badge(text, color) {
	return E('span', {
		'style': 'display:inline-block;padding:1px 8px;border-radius:3px;color:#fff;font-weight:bold;background:' + color
	}, text);
}

function yesno(v, yes, no) {
	return v ? badge(yes, '#2e7d32') : badge(no, '#9e9e9e');
}

function duration(s) {
	if (s == null) return '-';
	var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
	return (h ? h + 'h ' : '') + (h || m ? m + 'm ' : '') + (s % 60) + 's';
}

function getStatus() {
	return fs.exec('/usr/bin/lora-bridge', [ 'status' ]).then(function(res) {
		return JSON.parse(res.stdout);
	}).catch(function() { return null; });
}

function renderStatus(st) {
	if (!st)
		return E('em', {}, _('Status not available.'));

	var cstate = st.container.exists ? st.container.state : _('not created');
	var ccolor = st.container.state == 'running' ? '#2e7d32' : (st.container.exists ? '#ef6c00' : '#9e9e9e');
	var rows = [
		[ _('Service'), st.service.running ? badge(_('running'), '#2e7d32')
			: (st.service.enabled ? badge(_('stopped'), '#c62828') : badge(_('disabled'), '#9e9e9e')) ],
		[ _('Device'), st.device.present
			? E('span', {}, [ badge(_('plugged in'), '#2e7d32'), ' %s %s (%s)'.format(st.device.product || '', st.device.serial || '', st.device.path) ])
			: badge(_('not present'), '#9e9e9e') ],
		[ _('Serial link'), st.link.attached
			? E('span', {}, [ badge(_('connected'), '#2e7d32'), ' %s → %s:%s, %s'.format(st.device.path, st.container.name, st.link.container_port, duration(st.link.connected_seconds)) ])
			: badge(_('not connected'), '#9e9e9e') ],
		[ _('Container'), E('span', {}, [ badge(cstate, ccolor), ' ', st.container.name,
			st.container.exists ? ' (%s, %s)'.format(st.container.image, st.container.id) : '' ]) ]
	];
	if (st.timer.stop_in != null)
		rows.push([ _('Auto stop'), badge(_('container stops in %s').format(duration(st.timer.stop_in)), '#ef6c00') ]);
	if (st.container.exists && !st.container.port_mounted)
		rows.push([ _('Warning'), _('The container does not mount the port directory. Recreate it or add the volume by hand.') ]);
	if (st.error)
		rows.push([ _('Last error'), E('span', { 'style': 'color:#c62828' }, st.error) ]);

	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '25%' }, r[0]),
			E('td', { 'class': 'td left' }, r[1])
		]);
	}));
}

function action(cmd, okmsg) {
	return function() {
		return fs.exec('/usr/bin/lora-bridge', [ cmd ]).then(function(res) {
			if (res.code !== 0)
				ui.addNotification(null, E('p', {}, (res.stderr || res.stdout || _('Command failed')).trim()), 'error');
			else
				ui.addNotification(null, E('p', {}, okmsg), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', {}, e.message), 'error');
		});
	};
}

return view.extend({
	load: function() {
		return getStatus();
	},

	render: function(status) {
		var m, s, o;

		m = new form.Map('lora-bridge', _('LoRa Bridge'),
			_('Starts (or creates) a Docker container when the USB LoRa bridge is plugged in and connects a virtual serial port inside the container to it. Some time after the device was unplugged the container is stopped; plugging it back in before that just reconnects.'));

		s = m.section(form.NamedSection, 'main', 'main', _('Device'));
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.rmempty = false;

		o = s.option(form.Value, 'usb_vid', _('USB vendor ID'));
		o.placeholder = '239a';
		o.datatype = 'and(hexstring,length(4))';

		o = s.option(form.Value, 'usb_pid', _('USB product ID'), _('Empty: any product of this vendor.'));
		o.placeholder = '00b3';
		o.datatype = 'and(hexstring,length(4))';
		o.rmempty = true;

		o = s.option(form.Value, 'usb_serial', _('USB serial number'), _('Only use the device with this serial number. Empty: the first one found.'));
		o.rmempty = true;

		o = s.option(form.ListValue, 'link_mode', _('Serial port in the container'));
		o.value('pty', _('Virtual serial port (stays open across re-plugging)'));
		o.value('device', _('Real device node (programs must reopen it after re-plugging)'));
		o.default = 'pty';

		o = s.option(form.Value, 'baud', _('Baud rate'));
		o.datatype = 'uinteger';
		o.placeholder = '115200';
		o.depends('link_mode', 'pty');

		o = s.option(form.Value, 'stop_delay', _('Stop delay (seconds)'),
			_('Time between unplugging and stopping the container. -1: never stop it.'));
		o.datatype = 'integer';
		o.placeholder = '300';

		s = m.section(form.NamedSection, 'container', 'container', _('Docker container'));
		s.addremove = false;

		o = s.option(form.Value, 'name', _('Container name'));
		o.rmempty = false;
		o.datatype = 'string';

		o = s.option(form.Flag, 'create', _('Create if missing'),
			_('Create the container with the settings below when it does not exist. Otherwise an existing container of that name is only started and stopped.'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Value, 'image', _('Image'));
		o.placeholder = 'alpine:latest';
		o.depends('create', '1');

		o = s.option(form.Value, 'command', _('Command'), _('Empty: the default command of the image.'));
		o.depends('create', '1');
		o.rmempty = true;

		o = s.option(form.Value, 'mount_path', _('Port directory in the container'),
			_('The serial port appears as ttyLORA in this directory, e.g. /dev/lora/ttyLORA. It is also passed as environment variable LORA_BRIDGE_PORT.'));
		o.placeholder = '/dev/lora';

		o = s.option(form.Value, 'network', _('Network'));
		o.placeholder = 'bridge';
		o.depends('create', '1');
		o.rmempty = true;

		o = s.option(form.ListValue, 'restart_policy', _('Restart policy'));
		o.value('no', _('no'));
		o.value('on-failure', 'on-failure');
		o.value('unless-stopped', 'unless-stopped');
		o.depends('create', '1');

		o = s.option(form.Flag, 'privileged', _('Privileged'));
		o.depends('create', '1');

		o = s.option(form.DynamicList, 'env', _('Environment variables'), 'NAME=value');
		o.depends('create', '1');

		o = s.option(form.DynamicList, 'volume', _('Volumes'), '/host/path:/container/path');
		o.depends('create', '1');

		o = s.option(form.DynamicList, 'port', _('Published ports'), '8080:80');
		o.depends('create', '1');

		o = s.option(form.Value, 'extra_args', _('Extra arguments'), _('Appended to "docker create".'));
		o.depends('create', '1');
		o.rmempty = true;

		o = s.option(form.Value, 'stop_timeout', _('Stop timeout (seconds)'));
		o.datatype = 'uinteger';
		o.placeholder = '10';

		return m.render().then(function(node) {
			var box = E('div', { 'id': 'lora-bridge-status' }, renderStatus(status));
			var panel = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Status')),
				box,
				E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, [
					E('button', { 'class': 'btn cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, action('start', _('Container started.'))) }, _('Start container')), ' ',
					E('button', { 'class': 'btn cbi-button cbi-button-neutral', 'click': ui.createHandlerFn(this, action('stop', _('Container stopped.'))) }, _('Stop container')), ' ',
					E('button', { 'class': 'btn cbi-button cbi-button-negative', 'click': ui.createHandlerFn(this, action('recreate', _('Container removed; it is created again when the device is present.'))) }, _('Recreate container'))
				])
			]);
			node.insertBefore(panel, node.querySelector('.cbi-section'));

			poll.add(function() {
				return getStatus().then(function(st) {
					var el = document.getElementById('lora-bridge-status');
					if (el) {
						while (el.firstChild) el.removeChild(el.firstChild);
						el.appendChild(renderStatus(st));
					}
				});
			}, 3);

			return node;
		}.bind(this));
	}
});