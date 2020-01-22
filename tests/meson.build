tests = [
    'fprintd',
    'test_fprintd_utils',
]

foreach t: tests
    test(t,
        python3,
        args: meson.current_source_dir() / t + '.py',
        suite: ['daemon'],
        depends: [
            fprintd,
            fprintd_utils,
        ],
        env: [
            'G_DEBUG=fatal-criticals',
            'FPRINT_BUILD_DIR=' + meson.build_root() / 'src',
            'TOPSRCDIR=' + meson.source_root(),
        ],
    )
endforeach

add_test_setup('default_setup',
    is_default: true,
    env: [
        'G_SLICE=always-malloc',
        'MALLOC_CHECK_=2',
        'MALLOC_PERTURB_=55',
    ],
)

if find_program('valgrind', required: false).found()
    glib_share = glib_dep.get_pkgconfig_variable('prefix') / 'share' / glib_dep.name()
    glib_suppressions = glib_share + '/valgrind/glib.supp'
    add_test_setup('valgrind',
        env: [
            'G_SLICE=always-malloc',
            'VALGRIND=' + glib_suppressions,
        ],
        timeout_multiplier: 5
    )
endif

if get_option('pam')
    subdir('pam')
endif
