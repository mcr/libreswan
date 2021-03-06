FROM fedora:20
MAINTAINER "Antony Antony" <antony@phenome.org>
ENV container docker
RUN yum -y update; yum clean all
RUN yum -y install systemd; yum clean all; \
(cd /lib/systemd/system/sysinit.target.wants/; for i in *; do [ $i == systemd-tmpfiles-setup.service ] || rm -f $i; done); \
rm -f /lib/systemd/system/multi-user.target.wants/*;\
rm -f /etc/systemd/system/*.wants/*;\
rm -f /lib/systemd/system/local-fs.target.wants/*; \
rm -f /lib/systemd/system/sockets.target.wants/*udev*; \
rm -f /lib/systemd/system/sockets.target.wants/*initctl*; \
rm -f /lib/systemd/system/basic.target.wants/*;\
rm -f /lib/systemd/system/anaconda.target.wants/*;
RUN yum install -y wget vim-enhanced bison flex gmp-devel nss-devel nspr-devel nss-tools  gcc make unbound-libs ipsec-tools pexpect racoon2 nc6 unbound-devel fipscheck-devel libcap-ng-devel git pam-devel audit-libs-devel strace unbound pexpect strongswan net-tools bind-utils rpm-build nc psmisc lsof valgrind ElectricFence yum-utils screen xl2tpd curl-devel pam-devel redhat-rpm-config openldap-devel hping3 nmap mtr; yum clean all
RUN debuginfo-install -y ElectricFence audit-libs cyrus-sasl glibc keyutils krb5-libs ldns libcap-ng libcom_err libcurl libevent libgcc libidn libselinux libssh2 nspr nss nss-softokn nss-softokn-freebl nss-util openldap openssl-libs pam pcre python-libs sqlite unbound-libs xz-libs zlib gmp nspr nss ; yum clean all
RUN mkdir -p /home/build/libreswan
VOLUME ["/home/build/libreswan:/home/build/libreswan"]
RUN ln -s /home/build/libreswan/testing /testing
RUN echo " * soft core unlimited" >> /etc/security/limits.conf
RUN echo " DAEMON_COREFILE_LIMIT='unlimited'" >> /etc/sysconfig/pluto
#
#ssh
RUN yum -y install openssh-server
RUN yum -y install openssh-clients
RUN mkdir /root/.ssh
RUN mkdir /var/run/sshd
# create host keys
RUN ssh-keygen -b 1024 -t rsa -f /etc/ssh/ssh_host_key
RUN ssh-keygen -b 1024 -t rsa -f /etc/ssh/ssh_host_rsa_key
RUN ssh-keygen -b 1024 -t dsa -f /etc/ssh/ssh_host_dsa_key
# move public key to enable ssh keys login
# copy the file /root/.ssh/authorized_keys to cwd
ADD authorized_keys /root/.ssh/authorized_keys
RUN chmod 400 /root/.ssh/authorized_keys
RUN chown -R root:root /root/.ssh
RUN  systemctl enable sshd.service
# tell ssh to not use ugly PAM
RUN sed -i 's/UsePAM\syes/UsePAM no/' /etc/ssh/sshd_config
# make the terminal prettier
RUN echo 'export GIT_PS1_SHOWDIRTYSTATE=true' >> /root/.bash_profile
RUN echo 'export PS1="[\u@i\h] \w # "' >> /root/.bash_profile
RUN echo 'export EDITOR=vim' >> /root/.bash_profile
VOLUME [ "/sys/fs/cgroup" ]
CMD ["/usr/sbin/init"]
