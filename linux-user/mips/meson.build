syscall_nr_generators += {
  'mips': generator(sh,
                    arguments: [ meson.current_source_dir() / 'syscallhdr.sh', '@INPUT@', '@OUTPUT@', '@EXTRA_ARGS@',
                                 '','TARGET_SYSCALL_OFFSET'],
                    output: '@BASENAME@_nr.h')
}
