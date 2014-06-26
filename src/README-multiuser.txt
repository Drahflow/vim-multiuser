Architecture:
  The memline_T holds a local version which is modified by the client VIm.
  All changes are sent to a remote server which tries to apply them.
  The server holds a master copy of the document, distributing the
  changes when applied successfully. It holds a global version number which
  increases monotonically (64bit).

  The client parses the incoming changes when waiting for key input (only),
  atomically moving to new versions when they are completely transferred.

Network protocol:

  8 byte packet length
  4 byte command
  <data>

  Commands:
    Command 0: NOP
    Command 1: Request full transfer
    Command 2: Append line
    * 8 byte: Line number to append after
    * 8 byte: Data length
    * Data
    Command 4: Delete line
    * 8 byte: Line number to delete
    Command 5: Version accepted (incoming linenumbers now relative to version)
    * 8 byte: Last version synced into local view
    Command 6: Master Transfer complete
    * 8 byte: Version after change
    Command 8: Clear buffer
    Command 9: Checkpoint request
    * 8 byte: Checkpoint ID
    * 8 byte: Client lnum
    Command 10: Checkpoint reached
    * 8 byte: Checkpoint ID
    * 8 byte: Master (and new client) lnum

C-Vers S-Vers
1-----------1
2-----------2
3-----------3
4-----
5-----
6-----------4
7-----------5
       -----6
8-----------7

  The above mapping is kept for each version until a newer one has been
  accepted. The last accepted version gives the relevant line number mapping.
  The server sends linenumbers always according to the newest master version,
  mapping incoming line numbers as appropriate.

  Typical session (client left)
  -> Request full transfer
  <- Clear buffer
  <- Append line
     ...
  <- Append line
  <- Transfer complete V-123
  -> Version accepted V-123
  
  -> Replace line
  -> Slave Transfer complete 
  <- Replace line
  <- Master Transfer complete V-124
  -> Version accepted V-124

  The server version starts at 1, so 0 can be used to signify "no version".
