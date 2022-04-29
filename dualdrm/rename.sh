#!/bin/bash
echo '#ifndef _DRM_RENAME_SYMBOLS_H
#define _DRM_RENAME_SYMBOLS_H' > include/drm/rename-symbols.h
find drivers/gpu/drm -name '*.c' | xargs grep 'EXPORT_SYMBOL.*(' | \
  grep -v 'EXPORT_SYMBOL.*(i915_' | \
  sed -e's/^.*(\(.*\));.*$/#define \1 _ukmd_\1/g' >> include/drm/rename-symbols.h
echo '#endif' >> include/drm/rename-symbols.h

