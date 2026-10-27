# "Fake device" for dry-running the deploy/ install scripts on the PC (NOT a Raspberry Pi, NOT LG AI Native OS).
# sshd (root, key only) + a fake opkg that really unpacks the IPK and runs its postinst, and systemctl/journalctl
# stubs that only log. Binaries are aarch64 and are never executed here; this checks the scripts' file work.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      openssh-server binutils sqlite3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /run/sshd /var/lib/fake-opkg /root/.ssh && chmod 700 /root/.ssh \
    && groupadd -r network && useradd -r -g network -M -s /usr/sbin/nologin network && groupadd -r audio || true \
    && sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/; s/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config
COPY fake-opkg /usr/local/bin/opkg
COPY fake-systemctl /usr/local/bin/systemctl
COPY fake-journalctl /usr/local/bin/journalctl
RUN chmod 0755 /usr/local/bin/opkg /usr/local/bin/systemctl /usr/local/bin/journalctl
EXPOSE 22
CMD ["/usr/sbin/sshd", "-D", "-e"]
