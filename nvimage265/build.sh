#!/bin/bash

OPT="-O2"

cc -I. -I/opt/gst/gstreamer/subprojects/gst-plugins-base/gst-libs -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch $OPT -g -fvisibility=hidden -fno-strict-aliasing -DG_DISABLE_DEPRECATED -Wmissing-declarations -Wredundant-decls -Wwrite-strings -Winit-self -Wmissing-include-dirs -Wno-multichar -Wvla -Wpointer-arith -Wmissing-prototypes -Wdeclaration-after-statement -Wold-style-definition -Waggregate-return -fPIC -pthread -DHAVE_CONFIG_H -MD -MQ nvimageutil.c.o -MF nvimageutil.c.o.d -o nvimageutil.c.o -c nvimageutil.c

cc -I. -I/opt/gst/gstreamer/subprojects/gst-plugins-base/gst-libs -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch $OPT -g -fvisibility=hidden -fno-strict-aliasing -DG_DISABLE_DEPRECATED -Wmissing-declarations -Wredundant-decls -Wwrite-strings -Winit-self -Wmissing-include-dirs -Wno-multichar -Wvla -Wpointer-arith -Wmissing-prototypes -Wdeclaration-after-statement -Wold-style-definition -Waggregate-return -fPIC -pthread -DHAVE_CONFIG_H -MD -MQ gstnvimagesrc.c.o -MF gstnvimagesrc.c.o.d -o gstnvimagesrc.c.o -c gstnvimagesrc.c

cc  -o libgstnvimagesrchevc.so gstnvimagesrc.c.o nvimageutil.c.o -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libgstnvimagesrchevc.so -Wl,-Bsymbolic-functions /usr/lib/x86_64-linux-gnu/libgstbase-1.0.so /usr/lib/x86_64-linux-gnu/libgstreamer-1.0.so /usr/lib/x86_64-linux-gnu/libgobject-2.0.so /usr/lib/x86_64-linux-gnu/libglib-2.0.so /usr/lib/x86_64-linux-gnu/libgstvideo-1.0.so /usr/lib/x86_64-linux-gnu/libX11.so -lnvcuvid -lnvidia-encode -lnvidia-fbc -lGL -lpthread -Wl,--end-group