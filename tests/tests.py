#!/usr/bin/python

# This file is part of the flashrom project.
#
# Copyright (C) 2013 Google Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

import binascii
import os
import struct
import unittest

# Use this to find the FDT containing the flashmap
FDTMAP_SIGNATURE = '__FDTM__'

fdt_src = """
/dts-v1/;
/ {
    #address-cells = <0x00000001>;
    #size-cells = <0x00000001>;
    model = "NVIDIA Seaboard";
    compatible = "nvidia,seaboard", "nvidia,tegra250";
    interrupt-parent = <0x00000001>;
    flash@0 {
        #address-cells = <0x00000001>;
        #size-cells = <0x00000001>;
        compatible = "winbond,W25Q32BVSSIG", "cfi-flash", "chromeos,flashmap";
        reg = <0x00000000 0x00400000>;
        onestop-layout@0 {
            label = "onestop-layout";
            reg = <0x00000000 0x00080000>;
        };
        firmware-image@0 {
            label = "firmware-image";
            reg = <0x00000000 0x0007df00>;
        };
        verification-block@7df00 {
            label = "verification-block";
            reg = <0x0007df00 0x00002000>;
        };
        firmware-id@7ff00 {
            label = "firmware-id";
            reg = <0x0007ff00 0x00000100>;
        };
        readonly@0 {
            label = "readonly";
            reg = <0x00000000 0x00100000>;
            read-only;
        };
        bct@0 {
            label = "bct";
            reg = <0x00000000 0x00010000>;
            read-only;
        };
        ro-onestop@10000 {
            label = "ro-onestop";
            reg = <%(start)#x %(size)#x>;
            read-only;
            type = "blob boot";
        };
        ro-gbb@90000 {
            label = "gbb";
            reg = <0x00090000 0x00020000>;
            read-only;
            type = "blob gbb";
        };
        ro-data@b0000 {
            label = "ro-data";
            reg = <0x000b0000 0x00010000>;
            read-only;
        };
        ro-vpd@c0000 {
            label = "ro-vpd";
            reg = <0x000c0000 0x00008000>;
            read-only;
            type = "wiped";
            wipe-value = [ffffffff];
        };
        fdtmap {
            label = "ro-fdtmap";
            reg = <%(fdtmap_pos)#x %(fdtmap_size)#x>;
            read-only;
            type = "fdtmap";
        };
        fmap {
            label = "ro-fmap";
            reg = <0x000d0000 0x00000400>;
            read-only;
            type = "fmap";
            ver-major = <0x00000001>;
            ver-minor = <0x00000000>;
        };
        readwrite@100000 {
            label = "readwrite";
            reg = <0x00100000 0x00100000>;
        };
        rw-vpd@100000 {
            label = "rw-vpd";
            reg = <0x00100000 0x00080000>;
            type = "wiped";
            wipe-value = [ffffffff];
        };
        shared-dev-cfg@180000 {
            victoria;
            label = "shared-dev-cfg";
            reg = <0x00180000 0x00040000>;
            type = "wiped";
            wipe-value = "";
        };
        shared-data@1c0000 {
            label = "shared-data";
            reg = <0x001c0000 0x00030000>;
            type = "wiped";
            wipe-value = "";
        };
        shared-env@1ff000 {
            label = "shared-env";
            reg = <0x001ff000 0x00001000>;
            type = "wiped";
            wipe-value = "";
        };
        readwrite-a@200000 {
            label = "readwrite-a";
            reg = <0x00200000 0x00080000>;
            block-lba = <0x00000022>;
        };
        rw-a-onestop@200000 {
            label = "rw-a-onestop";
            reg = <0x00200000 0x00080000>;
            type = "blob boot";
        };
        readwrite-b@300000 {
            label = "readwrite-b";
            reg = <0x00300000 0x00080000>;
            block-lba = <0x00000422>;
        };
        rw-b-onestop@300000 {
            label = "rw-b-onestop";
            reg = <0x00380000 0x00008000>;
            type = "blob boot";
        };
    };
    config {
        silent_console = <0x00000000>;
        odmdata = <0x300d8011>;
        hwid = "ARM SEABOARD TEST 1176";
        machine-arch-id = <0x00000bbd>;
        gpio_port_write_protect_switch = <0x0000003b>;
        gpio_port_recovery_switch = <0x00000038>;
        gpio_port_developer_switch = <0x000000a8>;
        polarity_write_protect_switch = <0x00000001>;
        polarity_recovery_switch = <0x00000000>;
        polarity_developer_switch = <0x00000001>;
    };
};
"""

flash_size = 4 * 1024 * 1024
fdtmap_size = 0x00008000
src_fname = 'test.dts'
dtb_fname = 'test.dtb'
image_fname = 'image.bin'
part_fname = 'part.bin'
start = 0x10000
size = 0x80000


def Run(args):
  """Run a command + args and raise if it fails.

  Args:
    args: List of arguments, first is the program to run.

  Raises:
    OSError: Raised if the command fails.
  """
  cmd = ' '.join(args)
  print cmd
  if os.system(cmd):
    raise OSError("Command '%s' failed" % cmd)


class TestFlashrom(unittest.TestCase):
  """Unit test class for flashrom."""

  def setUp(self):
    """Set up read to run some tests.

    Create a chunk of data to be writte to the image, in self.part.
    """
    # Create some dummy data.
    part = ''
    for i in range(size):
      part += '%d.' % i
      if len(part) >= size:
        break
    part = part[:size]
    part_image = chr(0xff) * start
    part_image += part + chr(0xff) * (flash_size - size - start)
    open(part_fname, 'wb').write(part_image)
    self.part = part

  def InsertData(self, image, pos, data):
    """Insert some data into an image.

    Args:
      image: String containing input image.
      pos: Position to place data.
      data: Data to place (a string).
    Returns:
      String containing updated image.
    """
    return image[:pos] + data + image[pos + len(data):]

  def GetHeader(self, sig, data, bad_crc=False):
    """Create a suitable header for an FDTMAP.

    Args:
      sig: Sigature to use (string).
      data: Data to write (only the length is used here).
      bad_crc: True to force the header to have a bad CRC.
    Returns:
      FDTMAP header for the given data.
    """
    crc32 = binascii.crc32(data) & 0xffffffff
    if bad_crc:
      crc32 += 1
    return struct.pack('<8sLL', sig, len(data), crc32)

  def TryTest(self, sig=FDTMAP_SIGNATURE, fdtmap_pos=0x000c8000,
              bad_crc=False, bad_size=False, decoy=False):
    """Simple test to check that an FDT flashmap works correctly.

    Create an FDT map and write it to an image file. Then write a single
    part of that image, read it back and return it. This allows the caller
    to verify that the FDT map functionality works.

    Args:
      sig: Sigature to use (string).
      fdtmap_pos: Position in image where FDTMAP will go.
      bad_crc: True to force the header to have a bad CRC.
      bad_size: True to force the size field to be incorrect.
      decoy: True to create lots of decoy blocks with invalid signatures or
          CRCs.
    Returns:
      Contents of the part that was read back from the created image.
    """
    # Create the FDT source file, then compile it.
    props = {
        'start': start,
        'size': size,
        'fdtmap_pos': fdtmap_pos,
        'fdtmap_size': fdtmap_size,

    }
    with open(src_fname, 'w') as fd:
      print >>fd, fdt_src % props
    Run(['dtc', '-O', 'dtb', '-o', dtb_fname, src_fname])

    # Create an image and put the flashmap in it.
    image = chr(0) * flash_size
    with open(dtb_fname, 'r') as fd:
      data = fd.read()
    if bad_size:
      data = data[4:]
    fdtmap = self.GetHeader(sig, data, bad_crc)
    if decoy:
      for upto in range(0, flash_size, 0x20000):
        image = self.InsertData(image, upto, fdtmap + data[:-4] + 'junk')
        bad_data = self.GetHeader(sig, data[0x100:], True) + data[0x100:]
        image = self.InsertData(image, upto + 0x10000, bad_data)
    image = self.InsertData(image, fdtmap_pos, fdtmap + data)
    with open(image_fname, 'wb') as fd:
      fd.write(image)

    # Use flashrom to write it into the image.
    args = ['sudo', '../flashrom',
            '-p', 'dummy:emulate=SST25VF032B,image=%s' % image_fname,
            '-w', part_fname, '-i', 'RO_ONESTOP']
    Run(args)

    # Read it back.
    os.unlink(part_fname)
    args[4] = '-r'
    Run(args)

    # Make sure that it matches.
    with open(part_fname, 'rb') as fd:
      check = fd.read()
    check = check[start:start + size]
    Run(['sudo', 'rm', '-f', part_fname])
    return check

  def testValidmap(self):
    """Simple test with fairly well aligned fdtmap."""
    self.assertEqual(self.part, self.TryTest())

  def testExhaustiveSearch(self):
    """Test that the exhaustive search works OK."""
    self.assertEqual(self.part, self.TryTest(fdtmap_pos=0x000c8001))

  def testInvalidSignature(self):
    """Make sure that a bad signature is detected."""
    self.assertRaises(OSError, self.TryTest, sig='bad sig!')

  def testInvalidCRC(self):
    """Make sure that an invalid CRC32 is detected."""
    self.assertRaises(OSError, self.TryTest, bad_crc=True)

  def testInvalidSize(self):
    """Make sure that an invalid FDT size is detected."""
    self.assertRaises(OSError, self.TryTest, bad_size=True)

  def testDecoy(self):
    """Make sure that decoys (bad FDT maps) don't throw us off."""
    self.assertEqual(self.part, self.TryTest(decoy=True))

if __name__ == '__main__':
  print 'Testing fdtmap'
  unittest.main()
