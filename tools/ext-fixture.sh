# FolioOS - makes one ext4 test volume with Linux's own tools (run inside Alpine by tools\ext-fixtures.ps1)
# __NAME__ = clean|small|errors|recover|linux; __DEV__ = the partition (/dev/vda1, or /dev/vda2 after an EFI partition).
# Prints the facts the test compares with FolioOS's reader: "facts: ..." (block size, blocks, last used block).
set -e
name=__NAME__
dev=__DEV__
case $name in
  small) mkfs.ext4 -q -F -L FOLIOTEST -b 1024 $dev ;;     # 1 KiB blocks, like mke2fs does for small disks
  *)     mkfs.ext4 -q -F -L FOLIOTEST -b 4096 $dev ;;     # 4 KiB blocks, like a Linux system disk
esac
mkdir -p /mnt/t && mount -t ext4 $dev /mnt/t
# files spread over the volume: many in one folder (htree index), some deleted, a big one at the end
mkdir -p /mnt/t/home/user/Documents /mnt/t/etc
i=0; while [ $i -lt 120 ]; do head -c $(( (i % 37 + 1) * 1024 )) /dev/zero | tr '\0' 'f' > /mnt/t/home/user/Documents/file$i.txt; i=$((i+1)); done
i=0; while [ $i -lt 120 ]; do rm /mnt/t/home/user/Documents/file$i.txt; i=$((i+3)); done
head -c 6000000 /dev/zero | tr '\0' 'b' > /mnt/t/big.bin
echo "FolioOS fixture" > /mnt/t/etc/hostname
ln -s /etc/hostname /mnt/t/link
sync; umount /mnt/t
case $name in
  errors)  debugfs -w -R "ssv state 3" $dev >/dev/null 2>&1 ;;              # valid + errors, as the kernel marks it
  recover) debugfs -w -R "feature needs_recovery" $dev >/dev/null 2>&1 ;;  # journal not replayed (not unmounted cleanly)
esac
e2fsck -fn $dev >/dev/null 2>&1 && echo "e2fsck: clean" || echo "e2fsck: rc=$?"
bs=$(dumpe2fs -h $dev 2>/dev/null | awk -F: '/^Block size/ {gsub(/ /,"",$2); print $2}')
bc=$(dumpe2fs -h $dev 2>/dev/null | awk -F: '/^Block count/ {gsub(/ /,"",$2); print $2}')
feat=$(dumpe2fs -h $dev 2>/dev/null | awk -F: '/^Filesystem features/ {sub(/^ +/,"",$2); gsub(/ /,",",$2); print $2}')
# last block in use: each group's range minus its free ranges (dumpe2fs lists both)
last=$(dumpe2fs $dev 2>/dev/null | awk '
  /^Group [0-9]+: \(Blocks / { s=$0; sub(/.*\(Blocks /,"",s); sub(/\).*/,"",s); split(s,r,"-"); ga=r[1]+0; gb=r[2]+0; next }
  /^  Free blocks: / { s=$0; sub(/^  Free blocks: */,"",s); n=split(s,parts,", *");
      # walk the group from the top down: the highest block not inside a free range
      b=gb; changed=1
      while (changed && b>=ga) { changed=0
        for (k=1;k<=n;k++) { if (parts[k]=="") continue; m=split(parts[k],x,"-"); lo=x[1]+0; hi=(m>1?x[2]:x[1])+0;
          if (b>=lo && b<=hi) { b=lo-1; changed=1 } } }
      if (b>=ga && b>last) last=b }
  END { print last+0 }')
echo "facts: name=$name bs=$bs blocks=$bc last=$last features=$feat"
