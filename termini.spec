#
# TERMINI specfile
#
# (C) Cyril Hrubis metan{at}ucw.cz 2013-2023
#

Summary: A terminal emulator
Name: termini
Version: git
Release: 1
License: GPL-2.0-or-later
Group: System/Utilities
Url: https://github.com/gfxprim/termini
Source: termini-%{version}.tar.bz2
BuildRequires: libgfxprim-devel
BuildRequires: libvterm-devel

BuildRoot: %{_tmppath}/%{name}-%{version}-buildroot

%description
A terminal emulator based on libvterm library

%prep
%setup -n termini-%{version}

%build
./configure.sh
make %{?jobs:-j%jobs}

%install
DESTDIR="$RPM_BUILD_ROOT" make install

%files -n termini
%defattr(-,root,root)
%{_bindir}/termini
%{_datadir}/applications/termini.desktop
%{_datadir}/termini/
%{_datadir}/termini/termini.png

%changelog
* Tue Jul 25 2023 Cyril Hrubis <metan@ucw.cz>

  Initial version.
