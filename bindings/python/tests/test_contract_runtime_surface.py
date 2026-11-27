"""The contract Protocols declare every public member of the runtime sockets.

Spec: bindings/doc/spec/README.ko.md (contract/runtime split) - a contract
that omits a public runtime member lets callers depend on something the spec
does not name.
"""

import unittest

import zlink

_FACTORIES = {
    "PairSocket": "create_pair_socket",
    "DealerSocket": "create_dealer_socket",
    "RouterSocket": "create_router_socket",
    "PubSocket": "create_pub_socket",
    "SubSocket": "create_sub_socket",
    "XPubSocket": "create_xpub_socket",
    "XSubSocket": "create_xsub_socket",
    "StreamSocket": "create_stream_socket",
}


def _declared(protocol):
    names = set()
    for klass in protocol.__mro__:
        if klass is object:
            continue
        names.update(vars(klass))
        names.update(getattr(klass, "__annotations__", {}))
    return {name for name in names if not name.startswith("_")}


def _public_members(instance):
    return {name for name in dir(type(instance)) if not name.startswith("_")}


class ContractRuntimeSurfaceTests(unittest.TestCase):
    def test_protocols_declare_every_public_runtime_member(self):
        context = zlink.create_context()
        try:
            missing = {}
            for protocol_name, factory_name in _FACTORIES.items():
                protocol = getattr(zlink, protocol_name)
                socket = getattr(zlink, factory_name)(context)
                try:
                    absent = _public_members(socket) - _declared(protocol)
                finally:
                    socket.close()
                if absent:
                    missing[protocol_name] = sorted(absent)
            self.assertEqual(missing, {})
        finally:
            context.close()


if __name__ == "__main__":
    unittest.main()
