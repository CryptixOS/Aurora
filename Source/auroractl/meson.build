#*
#* Created by v1tr10l7 on 27.01.2026.
#* Copyright (c) 2024-2026, Szymon Zemke <v1tr10l7@proton.me>
#*
#* SPDX-License-Identifier: GPL-3
#*/

executable(
  'auroractl', 'main.cpp',
  include_directories: incs, 
  cpp_args: [
    '-DFMT_EXCEPTIONS=0',
  ],
  link_args: [
    '-lsupc++', '-lgcc',
     '-lgcc_s'
  ],
  dependencies: deps, install: true,
  install_dir: '/usr/sbin/',
)
