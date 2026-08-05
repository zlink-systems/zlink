## Overview

zlink is a modern messaging library that provides asynchronous message
queues, multiple messaging patterns, message filtering (subscriptions),
and seamless access to multiple transport protocols including TCP, IPC,
inproc, WebSocket, and TLS.

This documentation describes the internal software that makes up the
zlink C++ core engine, and not how to use its API, however it may help
you understand certain aspects better, such as the callgraph of an API method.
There are no instructions on using zlink within this documentation, only
the API internals that make up the software.

**Note:** this documentation is generated directly from the source code with
Doxygen. Since this project is constantly under active development, what you
are about to read may be out of date! If you notice any errors in the
documentation, or the code comments, then please send a pull request.

Please refer to the README file for anything else.

## Resources

* Repository: https://github.com/kairos-code-dev/zlink

## Copyright

Copyright (c) 2007-2024 Contributors as noted in the AUTHORS file.
The project license is specified in LICENSE.
