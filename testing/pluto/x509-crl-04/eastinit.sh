/testing/guestbin/swan-prep --x509
echo "192.9.4.245 nic.testing.libreswan.org" >> /etc/hosts
cp /testing/x509/crls/cacrlvalid.crl /etc/ipsec.d/crls
certutil -D -n west -d /etc/ipsec.d
iptables -A INPUT -i eth1 -s 192.0.1.0/24 -d 0.0.0.0/0 -j DROP
ipsec setup start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-x509-cr
