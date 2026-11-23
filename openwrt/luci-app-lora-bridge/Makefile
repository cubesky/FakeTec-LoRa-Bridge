# luci-app-lora-bridge: start a Docker container when the FakeTec LoRa bridge is plugged in
# and connect a virtual serial port inside the container to it.
# Shell + LuCI JavaScript only, so one package fits every architecture.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-lora-bridge
PKG_VERSION:=1.1.0
PKG_RELEASE:=1
PKG_LICENSE:=AGPL-3.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=LoRa Bridge

LUCI_TITLE:=LuCI app: attach the LoRa serial bridge to a Docker container
LUCI_DESCRIPTION:=Starts (or creates) a Docker container when the USB LoRa bridge is plugged in, \
	connects a virtual serial port inside the container to it and stops the container \
	some time after the device was unplugged.
LUCI_DEPENDS:=+luci-base +socat +kmod-usb-acm
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature