{
  "targets": [
    {
      "target_name": "sctp_dgram",
      "sources": ["sctp_dgram.c"],
      "libraries": ["-lsctp"],
      "defines": ["NAPI_VERSION=8"]
    }
  ]
}
