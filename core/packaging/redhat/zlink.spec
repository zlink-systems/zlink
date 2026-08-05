# To build with draft APIs, use "--with drafts" in rpmbuild for local builds or add
#   Macros:
#   %_with_drafts 1
# at the BOTTOM of the OBS prjconf
%bcond_with drafts
%if %{with drafts}
%define DRAFTS yes
%else
%define DRAFTS no
%endif
%define lib_name libzlink11
Name:          zlink
Version:       11.2.0
Release:       1%{?dist}
Summary:       The Zlink messaging library
Group:         Development/Libraries/C and C++
License:       MPL-2.0
URL:           https://github.com/kairos-code-dev/zlink
Source:        https://github.com/kairos-code-dev/zlink/archive/v%{version}.tar.gz
Prefix:        %{_prefix}
Buildroot:     %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires:  autoconf automake libtool glib2-devel libbsd-devel
%if ! (0%{?fedora} > 12 || 0%{?rhel} > 5)
BuildRequires:  e2fsprogs-devel
BuildRoot:      %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
%endif
%bcond_with nss
%if %{with nss}
%if 0%{?suse_version}
BuildRequires:  mozilla-nss-devel
%else
BuildRequires:  nss-devel
%endif
%define NSS yes
%else
%define NSS no
%endif
%bcond_with tls
%if %{with tls} && ! 0%{?centos_version} < 700
%if 0%{?suse_version}
BuildRequires:  libgnutls-devel
%else
BuildRequires:  gnutls-devel
%endif
%define TLS yes
%else
%define TLS no
%endif
BuildRequires: gcc, make, gcc-c++, libstdc++-devel, asciidoc, xmlto
Requires:      libstdc++

%ifarch pentium3 pentium4 athlon i386 i486 i586 i686 x86_64
%{!?_with_pic: %{!?_without_pic: %define _with_pic --with-pic}}
%{!?_with_gnu_ld: %{!?_without_gnu_ld: %define _with_gnu_ld --with-gnu_ld}}
%endif

# We do not want to ship libzlink.la
%define _unpackaged_files_terminate_build 0

%description
zlink is a modern messaging library that provides asynchronous message
queues, multiple messaging patterns, message filtering (subscriptions),
and seamless access to multiple transport protocols including TCP, IPC,
inproc, WebSocket, and TLS.

%package -n %{lib_name}
Summary:   Shared Library for Zlink
Group:     Productivity/Networking/Web/Servers
Conflicts: zlink

%description -n %{lib_name}
zlink is a modern messaging library that provides asynchronous message
queues, multiple messaging patterns, message filtering (subscriptions),
and seamless access to multiple transport protocols including TCP, IPC,
inproc, WebSocket, and TLS.

This package contains the Zlink shared library.

%package devel
Summary:  Development files and static library for the Zlink library
Group:    Development/Libraries
Requires: %{lib_name} = %{version}-%{release}, pkgconfig
%bcond_with nss
%if %{with nss}
%if 0%{?suse_version}
Requires:  mozilla-nss-devel
%else
Requires:  nss-devel
%endif
%endif
%bcond_with tls
%if %{with tls} && ! 0%{?centos_version} < 700
%if 0%{?suse_version}
Requires:  libgnutls-devel
%else
Requires:  gnutls-devel
%endif
%endif

%description devel
zlink is a modern messaging library that provides asynchronous message
queues, multiple messaging patterns, message filtering (subscriptions),
and seamless access to multiple transport protocols including TCP, IPC,
inproc, WebSocket, and TLS.

This package contains Zlink related development libraries and header files.

%prep
%setup -q

%build
# Workaround for automake < 1.14 bug
mkdir -p config
autoreconf -fi
%configure --enable-drafts=%{DRAFTS} \
    --with-nss=%{NSS} \
    --with-tls=%{TLS} \
    %{?_with_pic} \
    %{?_without_pic} \
    %{?_with_gnu_ld} \
    %{?_without_gnu_ld}

%{__make} %{?_smp_mflags}

%check
%{__make} check VERBOSE=1

%install
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}
# Install the package to build area
%makeinstall

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%clean
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%files -n %{lib_name}
%defattr(-,root,root,-)

# docs in the main package
%doc LICENSE CHANGELOG.md

# libraries
%{_libdir}/libzlink.so.*

%{_mandir}/man7/zlink.7.gz

%files devel
%defattr(-,root,root,-)
%{_includedir}/zlink.h

%{_libdir}/libzlink.a
%{_libdir}/pkgconfig/libzlink.pc
%{_libdir}/libzlink.so

%{_mandir}/man3/zlink*
# skip man7/zlink.7.gz
%{_mandir}/man7/zlink_*

%changelog
* Tue Feb 11 2026 zlink maintainers <ulalax@kairoscode.dev>
- Fork from libzmq and rebrand as zlink
- Switch to CMake-only build system
- Add TLS/WSS transport support via Boost.Asio
