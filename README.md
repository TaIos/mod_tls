# mod_tls - memory safety for TLS in Apache

This repository contains `mod_tls`, a module for Apache httpd that uses
[rustls](https://github.com/ctz/rustls) to provide a memory safe TLS
implementation.

This project is sponsored by the [ISRG](https://www.abetterinternet.org). 
[Read what they said about it.](https://www.abetterinternet.org/post/memory-safe-tls-apache/).


## Status

In development. The module currently only works with an altered `crustls`(that
we plan to release soon) and the `trunk` version of the Apache httpd server.

Apache has received the first patches that allow two (or more) SSL providing modules
to be loaded and active on the same server. This required an extension of the core
API which is currently in trunk after review by the team. The goal is to propose
these changes for backport into a 2.4.x version when this becomes stabilized.

## Goals

Vital:

 * ```https:``` connectivity for Apache virtual hosts, supporting SNI and ALPN. (*Achieved*)
 * Own configuration directives with secure defaults. The module will not be a drop-in
   replacement for ```mod_ssl```. (*Ongoing*)
 * A test suite with a good coverage of the supported TLS features. (*Ongoing*)
 * A load test giving some performance indicators.
 * User documentation on how to deploy/configure.

Aimed for:

* Coexistence with ```mod_ssl```. There are setups where it is desirable to use ```mod_tls``` for frontend connections and ```mod_ssl``` for connections to backends. (*Ongoing*)
* Provide Certificates and OCSP Stapling via the [```mod_md```](https://github.com/icing/mod_md) module.
* Provide use of ```mod_tls``` for back backend https: connections.

## Platforms

 * Apache 2.4.x (trunk is necessary for now.)
 * OS: anything you can run apache and build rustls on
 * build system: autoconf/automake (for now)

### Installation from source

Run the usual autoconf/automake magic incantations. You need the Apache httpd development version (commonly called `apache2-dev` in distributions) and an installed crustls from <https://github.com/abetterinternet/crustls>.

***Caveat***: As development of `crustls` and `mod_tls` is ongoing, not every master/main branch will work with each other.

Run the usual autoconf/automake magic incantations.

```
> autoreconf -i
> automake
> autoconf
> ./configure --with-apxs=<path to apxs>
> make
```

### Test Suite

If you want to run the test suite, you need:

 * `curl` and `openssl` in your path
 * Some Python packages: `pytest`, `pyopenssl`

```
> make test
```