#ifndef EVO_SHADER_SCAN_H
#define EVO_SHADER_SCAN_H

/* Rip PSSL shader blobs from EVO's own loaded system modules -> /mnt/usb0/evo_shaders/.
 * Built only under -DEVO_SHADER_SCAN (package-app.sh --shader-scan). Runs once,
 * after the self-unjail. See evo_shader_scan.c. */
#ifdef EVO_SHADER_SCAN
void evo_shader_scan(void);
#else
#define evo_shader_scan() ((void)0)
#endif

#endif
