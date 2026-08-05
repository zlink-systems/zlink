# Per-Language Representation Of The Common Samples

[Common Samples](../README.ko.md)

The common samples' server roles, message names, DTO fields, state transitions, and verification
criteria are defined by each scenario document. The C++, .NET, Java, Kotlin, and Node.js
implementations may use whatever fits each language's grammar — `struct`, `record`, `class`,
`data class`, `interface`, and so on — but they don't change the meaning of the DTO names and
fields written in the common document.

Per-language documents aren't created by default. A language document is added under this directory
only when a difference is confirmed that makes it hard to build an identical implementation from the
common DTO table or common implementation criteria alone, such as:

- when nullability or a default value can't be distinguished by the language type alone
- when an enum's wire value or integer size needs to be fixed separately
- when a collection, timestamp, or identifier representation could change the common field's
  meaning
- when a declaration is needed to produce the same wire shape, because of the serializer's public
  behavior
- when a language runtime feature like reflection is absent, requiring a different fixed
  registration method

A per-language document records only the necessary DTO declarations and their mapping to common
fields, or an implementation representation that's unavoidable due to language characteristics. It
doesn't repeat run commands, project locations, server configuration, message flow, or verification
order. If a language needs no separate contract, no document is placed for it.

Currently, C++, .NET, Java, Kotlin, and Node.js's DTO representations have no additional per-language
contract. C++'s difference in explicit handler registration is already covered in the common sample
document and the C++ public contract, so it isn't explained again in this directory.
