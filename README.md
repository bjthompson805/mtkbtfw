# mtkbtfw

Firmware loader for MediaTek MT79xx USB Bluetooth adapters on FreeBSD.

These chips (the Bluetooth half of MT7925/MT7922/MT7921 Wi-Fi cards, common
in recent AMD laptops) refuse every HCI command until a patch is downloaded
into them. FreeBSD ships loaders for Intel (`iwmbtfw`) and Realtek
(`rtlbtfw`) parts but not MediaTek, so without this `dmesg` shows:

    ng_hci_process_command_complete: ubt0hci - HCI command failed, OGF=0x3, OCF=0x3, status=0xc

`mtkbtfw` is a userland port of the MT79xx path of Linux's
`drivers/bluetooth/btmtk.c` (ISC licensed, MediaTek), shaped after
`rtlbtfw(8)`. It needs no kernel changes.

## Status

| Chip   | USB ID      | Machine                      | State    |
|--------|-------------|------------------------------|----------|
| MT7925 | `13d3:3608` | ASUS Zenbook S 16 (UM5606WA) | Verified |

Tested on FreeBSD 15.1. After loading, pairing works, and so does A2DP
audio to a headset through `virtual_oss`. MT7922, MT7921 and MT7902 go
through the same code path in Linux and are handled, but untested. To try
one, add its USB ID to `mtk_list` in `mtkbtfw.c` and to `mtkbtfw.conf`, and
please report back.

## How it works

Everything runs over the USB control endpoint:

1. `ng_ubt(4)` attaches to the chip before any firmware is loaded, so the
   loader detaches it from interface 0.
2. It reads the chip ID and firmware version registers to pick the patch
   file, as Linux does.
3. It sends each patch section in 250-byte WMT commands wrapped in vendor
   HCI command `0xfc6f`, and polls each WMT event back with a vendor
   control IN request.
4. It enables the Bluetooth function, then resets the USB device so
   `ng_ubt` attaches again to a working chip. The patch survives the reset.

A rerun guard (`/var/run/mtkbtfw.<ugen>`) stops devd from looping if a
reset ever does wipe the patch.

## Install

Build and install the tool, man page and devd rule:

    make
    make install install-devd
    service devd restart

Fetch the firmware from linux-firmware, kept at its upstream relative path:

    mkdir -p /usr/local/share/mtkbt-firmware/mediatek/mt7925
    fetch -o /usr/local/share/mtkbt-firmware/mediatek/mt7925/ \
      https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7925/BT_RAM_CODE_MT7925_1_1_hdr.bin

devd then runs the loader at every boot and hotplug. To run it by hand
(`sysctl dev.ubt.0.%location` gives the ugen name):

    mtkbtfw -I -F -d ugen1.2
    hccontrol -n ubt0hci read_local_version_information

`-D` adds debug output. `-R` resets the Bluetooth subsystem first, for a
chip left in a bad state. See `mtkbtfw(8)`.

## Using it: a Bluetooth headset

Once the chip answers, it's ordinary FreeBSD Bluetooth. These were the
problems on the way to audio playing:

1. **Daemons and pairing.**

       sysrc hcsecd_enable=YES sdpd_enable=YES
       service hcsecd start; service sdpd start
       bluetooth-config scan

2. **Use PIN `0000`, not `nopin`.** FreeBSD's stack has no Secure Simple
   Pairing, so headsets fall back to legacy PIN pairing. With `nopin` the
   audio channel is refused with `Could not connect to HC: 13` (EACCES).
   Set `pin "0000";` for the device in `/etc/bluetooth/hcsecd.conf`,
   restart `hcsecd`, and put the headset in pairing mode.

3. **Give the device a short alias.** `virtual_oss` rejects
   `/dev/bluetooth/xx:xx:xx:xx:xx:xx` with "Device name too long". It looks
   names up in `/etc/bluetooth/hosts`, so append a short alias such as `hs`
   to the device's line there.

4. **Install the backend and use playback only.**

       pkg install virtual_oss_bluetooth

   `-f` also asks for an A2DP *source*. Headsets don't have one: their mic
   uses the Handsfree profile, which `virtual_oss` doesn't support. That
   fails with `PSM not found`, so use `-R /dev/null -P` instead. To run it
   at boot, set this in `/etc/rc.conf`:

       virtual_oss_enable="YES"
       virtual_oss_configs="bt"
       virtual_oss_bt="-C 2 -c 2 -r 48000 -b 16 -s 4ms -R /dev/null -P /dev/bluetooth/hs -d dsp.bt"

5. **Declare a PipeWire sink.** PipeWire's OSS scan only lists the kernel's
   `pcmN` devices, not the one `virtual_oss` creates. Add the sink in
   `~/.config/pipewire/pipewire.conf.d/bt-headset.conf`:

       context.objects = [
           { factory = adapter
               args = {
                   factory.name             = freebsd-oss.sink
                   node.name                = bt-headset
                   node.description         = "Bluetooth headset"
                   media.class              = Audio/Sink
                   api.freebsd-oss.dsp-path = /dev/dsp.bt
               }
           }
       ]

## License

ISC, the same as the Linux code it is ported from. The firmware belongs to
MediaTek, is distributed through linux-firmware under its own license, and
is not part of this repository.
