# query

devcon classfilter usb upper
pnputil /enum-devices /deviceid "USB\ROOT_HUB30" /stack

devcon hwids ROOT\USBIP_WIN2\*
pnputil /enum-devices /deviceid "ROOT\USBIP_WIN2\UDE" /stack /properties

pnputil /enum-classes /class usb /services
pnputil /enum-drivers /class usb /files

# install 

cd D:\usbip-win2\x64\Debug\package\

certutil -f -p usbip -importPFX root ..\..\..\drivers\package\usbip.pfx
rem certutil -f -p usbip -importPFX TrustedPublisher ..\..\..\drivers\package\usbip.pfx
certutil -store root | findstr "USBip"

pnputil /add-driver usbip2_filter.inf /install
..\classfilter add upper "{36FC9E60-C465-11CF-8056-444553540000}" usbip2_filter
..\devnode install usbip2_ide.inf ROOT\USBIP_WIN2\UDE
