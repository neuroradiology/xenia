test_vpkuwus128_1:
  # {0, 65536, 1, 65537}
  #_ REGISTER_IN v3 [00000000, 00010000, 00000001, 00010001]
  # {2, 65538, 3, 65539}
  #_ REGISTER_IN v4 [00000002, 00010002, 00000003, 00010003]
  vpkuwus128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [00000000, 00010000, 00000001, 00010001]
  #_ REGISTER_OUT v4 [00000002, 00010002, 00000003, 00010003]
  # {0, 65535, 1, 65535, 2, 65535, 3, 65535}
  #_ REGISTER_OUT v5 [0000FFFF, 0001FFFF, 0002FFFF, 0003FFFF]

test_vpkuwus128_2:
  # {2147483648, 2147483647, 2, 3}
  #_ REGISTER_IN v3 [80000000, 7FFFFFFF, 00000002, 00000003]
  # {4294967295, 65538, 4294967294, 16}
  #_ REGISTER_IN v4 [FFFFFFFF, 00010002, FFFFFFFE, 00000010]
  vpkuwus128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [80000000, 7FFFFFFF, 00000002, 00000003]
  #_ REGISTER_OUT v4 [FFFFFFFF, 00010002, FFFFFFFE, 00000010]
  # {65535, 65535, 2, 3, 65535, 65535, 65535, 16}
  #_ REGISTER_OUT v5 [FFFFFFFF, 00020003, FFFFFFFF, FFFF0010]
