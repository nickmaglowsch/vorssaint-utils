# Vendored Wayland protocol XML

`wlr-foreign-toplevel-management-unstable-v1.xml` is vendored verbatim from
[wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols)
(`unstable/wlr-foreign-toplevel-management-unstable-v1.xml`, interface version
3). It carries its own MIT-style copyright header, which must stay in the file.
It is vendored because wlr-protocols ships no Debian/Ubuntu package: on Ubuntu
24.04 `apt-cache search wlr-protocols` finds nothing, and the only copy on the
system comes from a Rust crate's source tree.

`ext-foreign-toplevel-list-v1.xml` is **not** vendored: it is part of
wayland-protocols (staging) from 1.32 onwards, and the build reads it from
`pkg-config --variable=pkgdatadir wayland-protocols`. Ubuntu 24.04's
wayland-protocols is 1.45 and ships it. Builds against an older
wayland-protocols simply lose the `ext` code path; the wlr path still works.
