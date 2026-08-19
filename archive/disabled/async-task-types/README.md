# Disabled asynchronous task types

The 2026-08-18 static audit removed the following reserved-only enum values and
their task-history presentation branches from the product binary:

- `FileEncrypt`
- `FileDecrypt`
- `LargeFileSave`

No production caller submitted these task types. Secure-document open/save is
currently owned by the synchronous security service, and S2/S3/B2/B3 are
read-only, so retaining these names made an unimplemented asynchronous path
look active. Restore them only together with a real caller, cancellation and
secret-redaction contract, and GUI task-center validation.
