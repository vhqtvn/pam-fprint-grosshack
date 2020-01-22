subdir('services')

tests = [
    'test_pam_fprintd',
]

foreach t: tests
    test(t,
        python3,
        args: meson.current_source_dir() / t + '.py',
        suite: ['PAM'],
        depends: [
            pam_fprintd,
            pam_service_file,
        ],
        env: [
            'TOPBUILDDIR=' + meson.build_root(),
            'TOPSRCDIR=' + meson.source_root(),
            'LD_PRELOAD=libpam_wrapper.so',
            'PAM_WRAPPER=1',
            'PAM_WRAPPER_SERVICE_DIR=' + meson.current_build_dir() / 'services',
            'G_DEBUG=fatal-warnings',
        ],
        timeout: 60,
    )
endforeach
