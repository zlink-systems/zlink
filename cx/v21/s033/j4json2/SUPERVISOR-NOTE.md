
# PM 11-13 21:35 — .NET is the reference implementation
.NET keeps the same contract (default JSON, padded base64 byte sequences, same bench DTO) with L4 loss 10-41% and L4÷binding 0.56-0.78 (lad-dotnet-s1). "Remaining cost is required by the contract" is not acceptable while .NET does the same contract cheaper.
In report.md add a table: for each cost on your language's typed JSON path (encode, base64, decode, copies, allocations), the .NET equivalent file:line under framework/languages/dotnet/src/Zlink.Framework/ (e.g. System.Text.Json Utf8JsonWriter/Reader writing base64 straight into a pooled buffer) and what .NET does less. Fold that difference into the fix proposal.
