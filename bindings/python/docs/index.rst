zlink Python Binding API Reference
===================================

.. automodule:: zlink
   :members:
   :undoc-members:
   :show-inheritance:

Shared socket methods
---------------------

.. automethod:: zlink.StreamSocket.bind
.. automethod:: zlink.StreamSocket.unbind
.. automethod:: zlink.StreamSocket.set_tls_server
.. automethod:: zlink.StreamSocket.set_tls_client
.. automethod:: zlink.DealerSocket.connection_attempts_snapshot

Shared message accessors
------------------------

.. automethod:: zlink.Received.first_part
.. automethod:: zlink.Received.single_part_or_throw
