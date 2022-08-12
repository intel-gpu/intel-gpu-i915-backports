#ifndef _BACKPORT_CEC_H
#define _BACKPORT_CEC_H

#include_next<media/cec.h>
#include<linux/version.h>

#define cec_fill_conn_info_from_drm LINUX_I915_BACKPORT(cec_fill_conn_info_from_drm)
static inline void cec_fill_conn_info_from_drm(struct cec_connector_info *conn_info,
		                                 const struct drm_connector *connector)
{
	memset(conn_info, 0, sizeof(*conn_info));
	conn_info->type = CEC_CONNECTOR_TYPE_DRM;
	conn_info->drm.card_no = connector->dev->primary->index;
	conn_info->drm.connector_id = connector->base.id;
}

#endif /* _BACKPORT_CEC_H */
