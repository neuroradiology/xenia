group("third_party")
project("aes_128")
  uuid("b50458bf-dd83-4c1a-8cad-61f5fbbfd720")
  kind("StaticLib")
  language("C")
  defines({
    "_LIB",
  })
  includedirs({
    "aes_128",
  })
  files({
    "aes_128/aes.h",
    "aes_128/unroll/aes.c",
  })
