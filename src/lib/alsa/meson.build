if not is_linux
  alsa_dep = dependency('', required: false)
  subdir_done()
endif

libasound_dep = dependency('alsa', version: '>= 1.2', required: get_option('alsa'))
if not libasound_dep.found()
  alsa_dep = dependency('', required: false)
  subdir_done()
endif

conf.set('ENABLE_ALSA', true)

alsa = static_library(
  'alsa',
  'Version.cxx',
  'Error.cxx',
  'AllowedFormat.cxx',
  'ChannelMap.cxx',
  'HwSetup.cxx',
  'NonBlock.cxx',
  include_directories: inc,
  dependencies: [
    libasound_dep,
    pcm_basic_dep,
    log_dep,
  ],
)

alsa_dep = declare_dependency(
  link_with: alsa,
  dependencies: [
    event_dep,
  ],
)
