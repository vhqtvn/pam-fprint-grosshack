subdir('services')

tests = [
    'test_pam_fprintd',
]

foreach t: tests
    python_tests += [
        {
            'name': t,
            'file': files(meson.current_source_dir() / t + '.py')[0],
            'env': [
                'TOPBUILDDIR=' + meson.build_root(),
                'TOPSRCDIR=' + meson.source_root(),
                'LD_PRELOAD=libpam_wrapper.so',
                'PAM_WRAPPER=1',
                'PAM_WRAPPER_DEBUGLEVEL=2',
                'PAM_WRAPPER_SERVICE_DIR=' + meson.current_build_dir() / 'services',
                'G_DEBUG=fatal-warnings',
            ],
            'depends': [
                pam_fprintd,
                pam_service_file,
            ],
            'suite': ['PAM'],
        }
    ]
endforeach
