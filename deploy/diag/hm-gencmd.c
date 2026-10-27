// hm-gencmd: one vcgencmd command over the VideoCore mailbox (/dev/vcio), for LG AI Native OS which ships no
// vcgencmd. Same request as raspberrypi/utils vcgencmd (tag GET_GENCMD_RESULT 0x00030080). Read-only commands only:
//   hm-gencmd pmic_read_adc EXT5V_V      hm-gencmd get_throttled      hm-gencmd measure_clock arm
// Build (PC, arm64 image): docker run --rm --platform linux/arm64 -v "$PWD/deploy/diag:/s" homemind-native-dev:24.04-arm64 \n//   gcc -O2 -Wall -Wextra -static -o /s/hm-gencmd /s/hm-gencmd.c   (binary not in git; copy to /tmp on the Pi, gone at reboot)
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)
#define MAX_STRING 1024

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: hm-gencmd COMMAND [ARG...]\n");
    return 2;
  }
  char command[MAX_STRING] = "";
  for (int a = 1; a < argc; ++a) {
    if (a > 1) strncat(command, " ", sizeof command - strlen(command) - 1);
    strncat(command, argv[a], sizeof command - strlen(command) - 1);
  }
  uint32_t p[(MAX_STRING >> 2) + 7];
  memset(p, 0, sizeof p);
  int i = 0;
  p[i++] = 0;           // buffer size, set below
  p[i++] = 0;           // process request
  p[i++] = 0x00030080;  // GET_GENCMD_RESULT
  p[i++] = MAX_STRING;  // value buffer size
  p[i++] = 0;           // request length
  p[i++] = 0;           // error code (response)
  memcpy(p + i, command, strlen(command) + 1);
  i += MAX_STRING >> 2;
  p[i++] = 0;  // end tag
  p[0] = i * sizeof *p;
  int fd = open("/dev/vcio", O_RDONLY);
  if (fd < 0) {
    perror("/dev/vcio");
    return 1;
  }
  if (ioctl(fd, IOCTL_MBOX_PROPERTY, p) < 0) {
    perror("mailbox");
    close(fd);
    return 1;
  }
  close(fd);
  printf("%s\n", (char *)(p + 6));
  return p[5] ? 1 : 0;
}
