=================================================
Development and test for Linux firewire subsystem
=================================================

2024/03/22
Takashi Sakamoto (坂本 貴史)
<o-takashi@sakamocchi.jp>

NOTICE
======

If you expect the previous state of this repository, please visit to
`master branch <https://github.com/takaswie/linux-firewire-dkms/tree/master>`_, however it is not
maintained anymore.

General
=======

Linux kernel has
`a subsystem to operate IEEE 1394 bus <https://ieee1394.docs.kernel.org/en/latest/>`_. This
repository is dedicated for its development and test.

Previously, this repository is named as ``snd-firewire-improve`` for the development and test of
ALSA firewire stack. You can see
`master branch <https://github.com/takaswie/linux-firewire-dkms/tree/master>`_ for the purpose,
however it is not integrated anymore.

Available kernel modules
========================

* subsystem core functions

  * firewire-core

* PCI device drivers

  * firewire-ohci
  * nosy

* Network protocol implementation and unit drivers

  * firewire-net

* SCSI protocol implementation and unit drivers

  * firewire-sbp2

* FireDTV/FloppyDTV protocol implementation and unit drivers

  * firedtv

* SCSI target protocol implementation and unit drivers

  * sbp_target

* Audio and music protocol implementation and unit drivers

  * snd-firewire-lib
  * snd-fireworks
  * snd-bebob
  * snd-oxfw
  * snd-dice
  * snd-firewire-digi00x
  * snd-firewire-tascam
  * snd-firewire-motu
  * snd-fireface

License
=======

GNU General Public License version 2.0, according to the license in Linux kernel.
