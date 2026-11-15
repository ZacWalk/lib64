@{
    schema = 1
    project = @{
        name = 'lib64'
        type = 'gui'
        'default-target' = 'app'
    }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{
            debug = 'debug'
            release = 'release'
        }
        'x64-linux' = @{
            debug = 'linux-debug'
            release = 'linux-release'
        }
    }
    targets = @(
        @{
            id = 'lib'
            kind = 'library'
            'cmake-target' = 'lib64'
            'test-label' = 'lib64'
            'debug-path' = 'build/{platform}/debug/{libprefix}lib64{lib}'
            'release-path' = 'build/{platform}/release/{libprefix}lib64{lib}'
            platforms = @('x64-windows', 'x64-linux')
        },
        @{
            id = 'app'
            kind = 'gui'
            'cmake-target' = 'app8bit'
            'test-label' = 'lib64'
            'debug-path' = 'bin/app8bit64d{exe}'
            'release-path' = 'bin/app8bit64r{exe}'
            platforms = @('x64-windows')
        }
    )
}
