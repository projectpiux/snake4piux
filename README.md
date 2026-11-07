# Snake4Piux

Snake4Piux (*Snake for Piux*) is a basic version of Snake written in C, built to run on [Piux](https://github.com/projectpiux/piux-kernel) as a freestanding ELF32 binary.

## Installation

### Manual installation

1. **Mount the disk image** `build/ext2.img`:

   ```bash
   sudo mkdir -p /mnt/piux
   sudo mount -o loop -t ext2 build/ext2.img /mnt/piux
   ```

2. **Open the mounted filesystem** in your file manager or terminal. Make sure to use `sudo`, since write access requires elevated privileges.

3. **Copy `snake.elf` into the mounted filesystem.** You can either download the prebuilt binary from the Releases page, or build it yourself:

   ```bash
   git clone https://github.com/projectpiux/snake4piux.git
   cd snake4piux
   gcc -m32 -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
       -nostdlib -Wall -Wextra -Werror=implicit-function-declaration \
       -march=i386 -O2 -fno-asynchronous-unwind-tables -fno-unwind-tables \
       -no-pie -Wl,-T,user/user.ld -Wl,-m,elf_i386 \
       -o snake.elf snake.c
   ```

   Then move the resulting `snake.elf` into the mounted filesystem.

4. **Unmount the filesystem:**

   ```bash
   sudo umount /mnt/piux
   ```

5. **Boot Piux.**

6. **Run the game** from the Piux terminal:

   ```
   run <path-to-snake.elf>
   ```

7. Enjoy! 🐍

### Pix Package Manager

Piux's package manager is not complete yet. Snake4Piux will be available in the Pix repositories at a later date.

## License

Snake4Piux is released under the [MIT License](LICENSE).
