load('//lib/builders.star', 'cpu', 'goma', 'os')
load('//lib/ci.star', 'ci')
load('//versioned/vars/ci.star', 'vars')
# Load this using relative path so that the load statement doesn't
# need to be changed when making a new milestone
load('../vars.star', milestone_vars='vars')

luci.bucket(
    name = vars.bucket.get(),
    acls = [
        acl.entry(
            roles = acl.BUILDBUCKET_READER,
            groups = 'all',
        ),
        acl.entry(
            roles = acl.BUILDBUCKET_TRIGGERER,
            groups = 'project-chromium-ci-schedulers',
        ),
        acl.entry(
            roles = acl.BUILDBUCKET_OWNER,
            groups = 'google/luci-task-force@google.com',
        ),
    ],
)

luci.gitiles_poller(
    name = vars.poller.get(),
    bucket = vars.bucket.get(),
    repo = 'https://chromium.googlesource.com/chromium/src',
    refs = [milestone_vars.ref],
)


ci.defaults.bucket.set(vars.bucket.get())
ci.defaults.bucketed_triggers.set(True)
ci.defaults.triggered_by.set([vars.poller.get()])


# Builders are sorted first lexicographically by the function used to define
# them, then lexicographically by their name


ci.android_builder(
    name = 'android-kitkat-arm-rel',
)

ci.android_builder(
    name = 'android-marshmallow-arm64-rel',
)


ci.chromiumos_builder(
    name = 'chromeos-amd64-generic-rel',
)

ci.chromiumos_builder(
    name = 'linux-chromeos-rel',
)


# This is launching & collecting entirely isolated tests.
# OS shouldn't matter.
ci.fyi_builder(
    name = 'mac-osxbeta-rel',
    goma_backend = None,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)


ci.fyi_windows_builder(
    name = 'Win10 Tests x64 1803',
    os = os.WINDOWS_10,
    goma_backend = None,
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)


ci.gpu_builder(
    name = 'Android Release (Nexus 5X)',
)

ci.gpu_builder(
    name = 'GPU Linux Builder',
)

ci.gpu_builder(
    name = 'GPU Mac Builder',
    cores = None,
    os = os.MAC_ANY,
)

ci.gpu_builder(
    name = 'GPU Win x64 Builder',
    builderless = True,
    os = os.WINDOWS_ANY,
)


ci.gpu_thin_tester(
    name = 'Linux Release (NVIDIA)',
    triggered_by = [vars.bucket.builder('GPU Linux Builder')],
)

ci.gpu_thin_tester(
    name = 'Mac Release (Intel)',
    triggered_by = [vars.bucket.builder('GPU Mac Builder')],
)

ci.gpu_thin_tester(
    name = 'Mac Retina Release (AMD)',
    triggered_by = [vars.bucket.builder('GPU Mac Builder')],
)

ci.gpu_thin_tester(
    name = 'Win10 x64 Release (NVIDIA)',
    triggered_by = [vars.bucket.builder('GPU Win x64 Builder')],
)


ci.linux_builder(
    name = 'Linux Builder',
)

ci.linux_builder(
    name = 'Linux Tests',
    goma_backend = None,
    triggered_by = [vars.bucket.builder('Linux Builder')],
)


ci.mac_builder(
    name = 'Mac Builder',
)

# The build runs on 10.13, but triggers tests on 10.10 bots.
ci.mac_builder(
    name = 'Mac10.10 Tests',
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

# The build runs on 10.13, but triggers tests on 10.11 bots.
ci.mac_builder(
    name = 'Mac10.11 Tests',
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'Mac10.12 Tests',
    os = os.MAC_10_12,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'Mac10.13 Tests',
    os = os.MAC_10_13,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)

ci.mac_builder(
    name = 'WebKit Mac10.13 (retina)',
    os = os.MAC_10_13,
    triggered_by = [vars.bucket.builder('Mac Builder')],
)


ci.mac_ios_builder(
    name = 'ios-simulator',
)


ci.win_builder(
    name = 'Win 7 Tests x64 (1)',
    os = os.WINDOWS_7,
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)

ci.win_builder(
    name = 'Win x64 Builder',
    cores = 32,
    os = os.WINDOWS_ANY,
)

ci.win_builder(
    name = 'Win10 Tests x64',
    triggered_by = [vars.bucket.builder('Win x64 Builder')],
)
