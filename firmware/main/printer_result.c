#include "printer_result.h"
#include "cJSON.h"

static bool printer_identity(cJSON *p, const peripage_info_t *info) {
    return cJSON_AddStringToObject(p, "name", info->name) &&
        cJSON_AddStringToObject(p, "model", "PeriPage A6+") &&
        cJSON_AddStringToObject(p, "firmware", info->firmware) &&
        cJSON_AddStringToObject(p, "hardware", info->hardware);
}

static bool printer_geometry(cJSON *p, const peripage_info_t *info) {
    return cJSON_AddNumberToObject(p, "dpi", info->dpi) &&
        cJSON_AddNumberToObject(p, "feed_dpi", info->feed_dpi) &&
        cJSON_AddNumberToObject(p, "width_dots", info->width_dots) &&
        cJSON_AddNumberToObject(p, "printable_width_mm", info->width_dots * 25.4 / info->dpi) &&
        cJSON_AddNumberToObject(p, "battery_pct", info->battery_pct);
}

static bool printer_charging(cJSON *p, const peripage_info_t *info) {
    return info->charging < 0 ? cJSON_AddNullToObject(p, "charging") :
        cJSON_AddBoolToObject(p, "charging", info->charging != 0);
}

static bool printer_fields(cJSON *root, const peripage_info_t *info) {
    cJSON *p = cJSON_AddObjectToObject(root, "printer");
    if (!p || !printer_identity(p, info) || !printer_geometry(p, info) || !printer_charging(p, info)) return false;
    cJSON *formats = cJSON_AddArrayToObject(p, "formats");
    if (!formats) return false;
    cJSON *text = cJSON_CreateString("text"), *qr = cJSON_CreateString("qr");
    if (!text || !qr) { cJSON_Delete(text); cJSON_Delete(qr); return false; }
    cJSON_AddItemToArray(formats, text); cJSON_AddItemToArray(formats, qr);
    return cJSON_AddNumberToObject(p, "max_height_mm", PRINTER_LAYOUT_MAX_HEIGHT_MM) &&
        cJSON_AddNullToObject(p, "media_height_mm") && cJSON_AddStringToObject(p, "size_source", "model_profile");
}

static bool layout_fields(cJSON *root, const printer_layout_info_t *info) {
    cJSON *p = cJSON_AddObjectToObject(root, "layout");
    if (!p) return false;
    bool ok = cJSON_AddStringToObject(p, "format", info->format == PRINTER_FORMAT_QR ? "qr" : "text") &&
        cJSON_AddNumberToObject(p, "width_dots", info->width_dots) &&
        cJSON_AddNumberToObject(p, "height_dots", info->height_dots) &&
        cJSON_AddNumberToObject(p, "width_mm", info->width_mm) &&
        cJSON_AddNumberToObject(p, "height_mm", info->height_mm);
    if (info->format == PRINTER_FORMAT_TEXT) ok = ok && cJSON_AddNumberToObject(p, "font_mm", info->font_mm);
    return ok;
}

static bool result_fields(cJSON *root, const printer_job_t *job, const char *error) {
    bool ok = cJSON_AddStringToObject(root, "t", "printer_result") &&
        cJSON_AddStringToObject(root, "rid", job->rid) &&
        cJSON_AddStringToObject(root, "status", error ? "error" : job->printing ? "sent" : "ready");
    if (error) ok = ok && cJSON_AddStringToObject(root, "code", error);
    if (job->printing) {
        const char *outcome = !error ? "submitted" : job->dispatched ? "unknown" : "not_sent";
        ok = ok && cJSON_AddStringToObject(root, "outcome", outcome);
    }
    return ok;
}

char *printer_result_make(const printer_job_t *job, const char *error,
                         const peripage_info_t *printer, const printer_layout_info_t *layout) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    bool ok = result_fields(root, job, error);
    if (printer) ok = ok && printer_fields(root, printer);
    if (layout) ok = ok && layout_fields(root, layout);
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}
