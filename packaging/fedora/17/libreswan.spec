%global USE_FIPSCHECK true
%global USE_LIBCAP_NG true
%global USE_LABELED_IPSEC true
%global USE_CRL_FETCHING true
%global USE_DNSSEC true
%global USE_NM true
%global USE_LINUX_AUDIT true

%global _hardened_build 1

%global fipscheck_version 1.3.0
%global buildefence 0
%global development 0

#global prever rc1

Name: libreswan
Summary: IPsec implementation with IKEv1 and IKEv2 keying protocols
# version is generated in the release script
Version: IPSECBASEVERSION
Release: %{?prever:0.}1%{?prever:.%{prever}}%{?dist}
License: GPLv2
Url: https://www.libreswan.org/
Source: https://download.libreswan.org/%{name}-%{version}%{?prever}.tar.gz
Group: System Environment/Daemons
BuildRequires: gmp-devel bison flex redhat-rpm-config pkgconfig
BuildRequires: systemd systemd-units
Requires(post): coreutils bash systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

Conflicts: openswan < %{version}-%{release}
Obsoletes: openswan < %{version}-%{release}
Provides: openswan = %{version}-%{release}

BuildRequires: pkgconfig hostname
BuildRequires: nss-devel >= 3.14.2-2, nspr-devel
BuildRequires: pam-devel
%if %{USE_DNSSEC}
BuildRequires: unbound-devel
%endif
%if %{USE_FIPSCHECK}
BuildRequires: fipscheck-devel >= %{fipscheck_version}
# we need fipshmac
Requires: fipscheck%{_isa} >= %{fipscheck_version}
%endif
%if %{USE_LINUX_AUDIT}
Buildrequires: audit-libs-devel
%endif

%if %{USE_LIBCAP_NG}
BuildRequires: libcap-ng-devel
%endif
%if %{USE_CRL_FETCHING}
BuildRequires: openldap-devel curl-devel
%endif
%if %{buildefence}
BuildRequires: ElectricFence
%endif
# Only needed if xml man pages are modified and need regeneration
BuildRequires: xmlto

Requires: nss-tools, nss-softokn
Requires: iproute >= 2.6.8

%description
Libreswan is a free implementation of IPsec & IKE for Linux.  IPsec is 
the Internet Protocol Security and uses strong cryptography to provide
both authentication and encryption services.  These services allow you
to build secure tunnels through untrusted networks.  Everything passing
through the untrusted net is encrypted by the ipsec gateway machine and 
decrypted by the gateway at the other end of the tunnel.  The resulting
tunnel is a virtual private network or VPN.

This package contains the daemons and userland tools for setting up
Libreswan. To build KLIPS, see the kmod-libreswan.spec file.

Libreswan also supports IKEv2 (RFC4309) and Secure Labeling

Libreswan is based on Openswan-2.6.38 which in turn is based on FreeS/WAN-2.04

%prep
%setup -q -n libreswan-%{version}%{?prever}
# remove man page for ipsec.conf so it is forced to regenerate
rm ./programs/configs/ipsec.conf.5

%build
%if %{buildefence}
 %define efence "-lefence"
%endif

#796683: -fno-strict-aliasing
%{__make} \
%if %{development}
   USERCOMPILE="-g -DGCC_LINT %(echo %{optflags} | sed -e s/-O[0-9]*/ /) %{?efence} -fPIE -pie -fno-strict-aliasing -Wformat-nonliteral -Wformat-security" \
%else
  USERCOMPILE="-g -DGCC_LINT %{optflags} %{?efence} -fPIE -pie -fno-strict-aliasing -Wformat-nonliteral -Wformat-security" \
%endif
  USERLINK="-g -pie -Wl,-z,relro,-z,now %{?efence}" \
  INITSYSTEM=systemd \
  USE_NM=%{USE_NM} \
  USE_XAUTHPAM=true \
  USE_FIPSCHECK="%{USE_FIPSCHECK}" \
  USE_LIBCAP_NG="%{USE_LIBCAP_NG}" \
  USE_LABELED_IPSEC="%{USE_LABELED_IPSEC}" \
%if %{USE_CRL_FETCHING}
  USE_LDAP=true \
  USE_LIBCURL=true \
%endif
  USE_DNSSEC="%{USE_DNSSEC}" \
  INC_USRLOCAL=%{_prefix} \
  FINALLIBEXECDIR=%{_libexecdir}/ipsec \
  MANTREE=%{_mandir} \
  INC_RCDEFAULT=%{_initrddir} \
  programs
FS=$(pwd)

%if %{USE_FIPSCHECK}
# Add generation of HMAC checksums of the final stripped binaries
%define __spec_install_post \
    %{?__debug_package:%{__debug_install_post}} \
    %{__arch_install_post} \
    %{__os_install_post} \
  fipshmac -d %{buildroot}%{_libdir}/fipscheck ` ls %{buildroot}%{_libexecdir}/ipsec/*|grep -v setup` \
  fipshmac -d %{buildroot}%{_libdir}/fipscheck %{buildroot}%{_sbindir}/ipsec \
%{nil}
%endif

%install
rm -rf ${RPM_BUILD_ROOT}
%{__make} \
  DESTDIR=%{buildroot} \
  INC_USRLOCAL=%{_prefix} \
  FINALLIBEXECDIR=%{_libexecdir}/ipsec \
  MANTREE=%{buildroot}%{_mandir} \
  INC_RCDEFAULT=%{_initrddir} \
  INSTMANFLAGS="-m 644" \
  INITSYSTEM=systemd \
  install
FS=$(pwd)
rm -rf %{buildroot}/usr/share/doc/libreswan

install -d -m 0755 %{buildroot}%{_localstatedir}/run/pluto
# used when setting --perpeerlog without --perpeerlogbase 
install -d -m 0700 %{buildroot}%{_localstatedir}/log/pluto/peer
install -d %{buildroot}%{_sbindir}

%if %{USE_FIPSCHECK}
mkdir -p %{buildroot}%{_libdir}/fipscheck
%endif

echo "include %{_sysconfdir}/ipsec.d/*.secrets" > %{buildroot}%{_sysconfdir}/ipsec.secrets
rm -fr %{buildroot}%{_sysconfdir}/rc.d/rc*

%files 
%doc CHANGES COPYING CREDITS README* LICENSE
%doc docs/*.* docs/examples
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/ipsec.conf
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/sysconfig/pluto
%attr(0600,root,root) %config(noreplace) %{_sysconfdir}/ipsec.secrets
%attr(0700,root,root) %dir %{_sysconfdir}/ipsec.d
%attr(0700,root,root) %dir %{_sysconfdir}/ipsec.d/cacerts
%attr(0700,root,root) %dir %{_sysconfdir}/ipsec.d/crls
%attr(0700,root,root) %dir %{_sysconfdir}/ipsec.d/policies
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/ipsec.d/policies/*
%attr(0700,root,root) %dir %{_localstatedir}/log/pluto/peer
%attr(0755,root,root) %dir %{_localstatedir}/run/pluto
%attr(0644,root,root) %{_unitdir}/ipsec.service
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/pam.d/pluto
%{_sbindir}/ipsec
%{_libexecdir}/ipsec
%attr(0644,root,root) %doc %{_mandir}/*/*

%if %{USE_FIPSCHECK}
%{_libdir}/fipscheck/*.hmac
%endif

%preun
if [ $1 -eq 0 ] ; then
    # Package removal, not upgrade
    /bin/systemctl --no-reload disable ipsec.service > /dev/null 2>&1 || :
    /bin/systemctl stop ipsec.service > /dev/null 2>&1 || :
fi

%postun
/bin/systemctl daemon-reload >/dev/null 2>&1 || :
if [ $1 -ge 1 ] ; then
    # Package upgrade, not uninstall
    /bin/systemctl try-restart ipsec.service >/dev/null 2>&1 || :
fi


%post 
if [ $1 -eq 1 ] ; then 
    # Initial installation 
    /bin/systemctl daemon-reload >/dev/null 2>&1 || :
fi
if [ ! -f %{_sysconfdir}/ipsec.d/cert8.db -a \
     ! -f %{_sysconfdir}/ipsec.d/cert9.db ] ; then
    certutil -N -d %{_sysconfdir}/ipsec.d --empty-password
    restorecon %{_sysconfdir}/ipsec.d/*db 2>/dev/null || :
fi


%changelog
* Tue Jan 01 2013 Team Libreswan <team@libreswan.org> - IPSEBASEVERSION-1
- Automated build from release tar ball
