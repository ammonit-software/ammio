#include "drivers/trdp_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/net.h"
#include "tau_xml.h"
#include "trdp_if_light.h"
#include "vos_mem.h"

typedef struct
{
    trdp_xml_var_t *vars;
    size_t n;
    size_t cap;
    uint32_t offset;
    int depth;
} flatten_t;

static bool push_var(flatten_t *f, const char *id, ammio_type_t type)
{
    if (f->n == f->cap)
    {
        size_t cap = f->cap ? f->cap * 2 : 32;
        trdp_xml_var_t *p = realloc(f->vars, cap * sizeof(*p));
        if (!p)
            return false;
        f->vars = p;
        f->cap = cap;
    }
    trdp_xml_var_t *v = &f->vars[f->n];
    v->var_id = ammio_strdup(id);
    if (!v->var_id)
        return false;
    v->type = type;
    v->byte_offset = f->offset;
    f->offset += (uint32_t)ammio_type_size_bytes(type);
    f->n++;
    return true;
}

static TRDP_DATASET_T *find_dataset(apTRDP_DATASET_T datasets, UINT32 n, UINT32 id)
{
    for (UINT32 i = 0; i < n; i++)
    {
        if (datasets[i] && datasets[i]->id == id)
            return datasets[i];
    }
    return NULL;
}

// Appends the elements of a dataset (recursively for nested datasets) in wire order
static bool flatten(flatten_t *f, TRDP_DATASET_T *ds, const char *prefix, apTRDP_DATASET_T datasets,
                    UINT32 n_datasets, const char *path, cfg_t *cfg)
{
    if (++f->depth > 16)
    {
        cfg_error(cfg, path, "dataset %u: nesting deeper than 16 levels (recursive dataset?)", ds->id);
        return false;
    }

    for (UINT16 e = 0; e < ds->numElement; e++)
    {
        const TRDP_DATASET_ELEMENT_T *el = &ds->pElement[e];
        char name[128];
        if (el->name && el->name[0])
            snprintf(name, sizeof(name), "%s", el->name);
        else
            snprintf(name, sizeof(name), "element%u", (unsigned)e);

        if (el->size == TRDP_VAR_SIZE)
        {
            cfg_error(cfg, path, "dataset %u element '%s': variable-size arrays are not supported", ds->id, name);
            return false;
        }

        for (UINT32 i = 0; i < el->size; i++)
        {
            char id[512];
            if (el->size > 1)
                snprintf(id, sizeof(id), "%s.%s[%u]", prefix, name, (unsigned)i);
            else
                snprintf(id, sizeof(id), "%s.%s", prefix, name);

            bool ok = true;
            char sub[600];
            switch (el->type)
            {
                case TRDP_BITSET8:     ok = push_var(f, id, AMMIO_TYPE_UINT8); break;  // also BOOL8 / ANTIVALENT8
                case TRDP_CHAR8:       ok = push_var(f, id, AMMIO_TYPE_UINT8); break;
                case TRDP_UTF16:       ok = push_var(f, id, AMMIO_TYPE_UINT16); break;
                case TRDP_INT8:        ok = push_var(f, id, AMMIO_TYPE_INT8); break;
                case TRDP_INT16:       ok = push_var(f, id, AMMIO_TYPE_INT16); break;
                case TRDP_INT32:       ok = push_var(f, id, AMMIO_TYPE_INT32); break;
                case TRDP_INT64:       ok = push_var(f, id, AMMIO_TYPE_INT64); break;
                case TRDP_UINT8:       ok = push_var(f, id, AMMIO_TYPE_UINT8); break;
                case TRDP_UINT16:      ok = push_var(f, id, AMMIO_TYPE_UINT16); break;
                case TRDP_UINT32:      ok = push_var(f, id, AMMIO_TYPE_UINT32); break;
                case TRDP_UINT64:      ok = push_var(f, id, AMMIO_TYPE_UINT64); break;
                case TRDP_REAL32:      ok = push_var(f, id, AMMIO_TYPE_FLOAT32); break;
                case TRDP_REAL64:      ok = push_var(f, id, AMMIO_TYPE_FLOAT64); break;
                case TRDP_TIMEDATE32:  ok = push_var(f, id, AMMIO_TYPE_UINT32); break;
                case TRDP_TIMEDATE48:
                    snprintf(sub, sizeof(sub), "%s.seconds", id);
                    ok = push_var(f, sub, AMMIO_TYPE_UINT32);
                    snprintf(sub, sizeof(sub), "%s.ticks", id);
                    ok = ok && push_var(f, sub, AMMIO_TYPE_UINT16);
                    break;
                case TRDP_TIMEDATE64:
                    snprintf(sub, sizeof(sub), "%s.seconds", id);
                    ok = push_var(f, sub, AMMIO_TYPE_UINT32);
                    snprintf(sub, sizeof(sub), "%s.microseconds", id);
                    ok = ok && push_var(f, sub, AMMIO_TYPE_INT32);
                    break;
                default:
                {
                    if (el->type <= TRDP_TYPE_MAX)
                    {
                        cfg_error(cfg, path, "dataset %u element '%s': unsupported type %u", ds->id, name, el->type);
                        return false;
                    }
                    TRDP_DATASET_T *nested = find_dataset(datasets, n_datasets, el->type);
                    if (!nested)
                    {
                        cfg_error(cfg, path, "dataset %u element '%s': dataset %u is not defined", ds->id, name, el->type);
                        return false;
                    }
                    if (!flatten(f, nested, id, datasets, n_datasets, path, cfg))
                        return false;
                    break;
                }
            }
            if (!ok)
            {
                cfg_error(cfg, path, "out of memory");
                return false;
            }
        }
    }

    f->depth--;
    return true;
}

static uint32_t parse_host(const TRDP_URI_HOST_T *uri)
{
    if (!uri || !(*uri)[0])
        return 0;
    struct in_addr addr;
    if (inet_pton(AF_INET, (const char *)*uri, &addr) != 1)
        return UINT32_MAX;  // hostname: not supported
    return ntohl(addr.s_addr);
}

int trdp_xml_import(const char *file, const char *interface_name, bool sut_perspective,
                    const char *path, cfg_t *cfg, trdp_xml_import_t *out)
{
    memset(out, 0, sizeof(*out));

    TRDP_XML_DOC_HANDLE_T doc;
    memset(&doc, 0, sizeof(doc));
    if (tau_prepareXmlDoc(file, &doc) != TRDP_NO_ERR)
    {
        // TCNopen allocates the handle before opening the file and does not free it on failure;
        // tau_freeXmlDoc() would try to close a file that was never opened.
        if (doc.pXmlDocument)
            vos_memFree(doc.pXmlDocument);
        cfg_error(cfg, path, "cannot read TRDP XML file '%s'", file);
        return -1;
    }

    int errors_before = cfg->errors;

    // Bus interface (for its host IP)
    TRDP_MEM_CONFIG_T mem_config;
    TRDP_DBG_CONFIG_T dbg_config;
    UINT32 n_com_par = 0, n_if = 0;
    TRDP_COM_PAR_T *com_par = NULL;
    TRDP_IF_CONFIG_T *if_config = NULL;
    memset(&mem_config, 0, sizeof(mem_config));
    memset(&dbg_config, 0, sizeof(dbg_config));
    bool found_if = false;
    if (tau_readXmlDeviceConfig(&doc, &mem_config, &dbg_config, &n_com_par, &com_par, &n_if, &if_config) == TRDP_NO_ERR)
    {
        for (UINT32 i = 0; i < n_if; i++)
        {
            if (strcmp(if_config[i].ifName, interface_name) == 0)
            {
                out->host_ip = if_config[i].hostIp;
                found_if = true;
            }
        }
    }
    if (com_par)
        vos_memFree(com_par);
    if (if_config)
        vos_memFree(if_config);
    if (!found_if)
    {
        cfg_error(cfg, path, "bus interface '%s' not found in '%s'", interface_name, file);
        tau_freeXmlDoc(&doc);
        return -1;
    }

    // Datasets
    UINT32 n_map = 0, n_datasets = 0;
    TRDP_COMID_DSID_MAP_T *map = NULL;
    apTRDP_DATASET_T datasets = NULL;
    if (tau_readXmlDatasetConfig(&doc, &n_map, &map, &n_datasets, &datasets) != TRDP_NO_ERR)
    {
        cfg_error(cfg, path, "cannot read the data-set-list of '%s'", file);
        tau_freeXmlDoc(&doc);
        return -1;
    }

    // Telegrams of the interface
    TRDP_PROCESS_CONFIG_T process_config;
    TRDP_PD_CONFIG_T pd_config;
    TRDP_MD_CONFIG_T md_config;
    UINT32 n_exchg = 0;
    TRDP_EXCHG_PAR_T *exchg = NULL;
    memset(&process_config, 0, sizeof(process_config));
    memset(&pd_config, 0, sizeof(pd_config));
    memset(&md_config, 0, sizeof(md_config));
    if (tau_readXmlInterfaceConfig(&doc, interface_name, &process_config, &pd_config, &md_config,
                                   &n_exchg, &exchg) != TRDP_NO_ERR)
    {
        cfg_error(cfg, path, "cannot read the telegrams of interface '%s' in '%s'", interface_name, file);
        tau_freeXmlDatasetConfig(n_map, map, n_datasets, datasets);
        tau_freeXmlDoc(&doc);
        return -1;
    }

    out->telegrams = calloc(n_exchg ? n_exchg : 1, sizeof(*out->telegrams));
    if (!out->telegrams)
        cfg_error(cfg, path, "out of memory");

    for (UINT32 t = 0; out->telegrams && t < n_exchg; t++)
    {
        const TRDP_EXCHG_PAR_T *x = &exchg[t];
        char tpath[CFG_PATH_MAX];
        snprintf(tpath, sizeof(tpath), "%s(comid %u)", path, x->comId);

        if (!x->pPdPar)
        {
            cfg_warn(cfg, tpath, "message-data telegram skipped (only process data is imported from XML)");
            continue;
        }

        // Direction of the telegram for the device the XML describes
        TRDP_EXCHG_OPTION_T type = x->type;
        if (type == TRDP_EXCHG_UNSET && out->host_ip != 0)
        {
            type = TRDP_EXCHG_SINK;
            for (UINT32 s = 0; s < x->srcCnt; s++)
            {
                if (parse_host(x->pSrc[s].pUriHost1) == out->host_ip)
                    type = TRDP_EXCHG_SOURCE;
            }
        }
        if (type != TRDP_EXCHG_SOURCE && type != TRDP_EXCHG_SINK)
        {
            cfg_error(cfg, tpath, "cannot tell whether the telegram is sent or received; set type=\"source\" or type=\"sink\" in the XML");
            continue;
        }

        bool device_sends = type == TRDP_EXCHG_SOURCE;
        // ammio plays the counterpart of the SUT
        bool ammio_publishes = sut_perspective ? !device_sends : device_sends;

        TRDP_DATASET_T *ds = find_dataset(datasets, n_datasets, x->datasetId);
        if (!ds)
        {
            cfg_error(cfg, tpath, "dataset %u is not defined", x->datasetId);
            continue;
        }

        trdp_xml_telegram_t *tel = &out->telegrams[out->n_telegrams];
        memset(tel, 0, sizeof(*tel));

        // Telegram id: dataset name, made unique with the comid if needed
        char id[300];
        snprintf(id, sizeof(id), "%s", ds->name[0] ? ds->name : "dataset");
        for (size_t k = 0; k < out->n_telegrams; k++)
        {
            if (strcmp(out->telegrams[k].id, id) == 0)
            {
                snprintf(id, sizeof(id), "%s_%u", ds->name[0] ? ds->name : "dataset", x->comId);
                break;
            }
        }

        flatten_t f;
        memset(&f, 0, sizeof(f));
        if (!flatten(&f, ds, id, datasets, n_datasets, tpath, cfg))
        {
            for (size_t k = 0; k < f.n; k++)
                free(f.vars[k].var_id);
            free(f.vars);
            continue;
        }

        tel->id = ammio_strdup(id);
        tel->dir = ammio_publishes ? AMMIO_DIR_INPUT : AMMIO_DIR_OUTPUT;
        tel->comid = x->comId;
        tel->period_ms = x->pPdPar->cycle / 1000u;
        tel->timeout_ms = x->pPdPar->timeout / 1000u;
        tel->size_bytes = f.offset;
        tel->vars = f.vars;
        tel->n_vars = f.n;

        uint32_t dest = x->destCnt > 0 ? parse_host(x->pDest[0].pUriHost) : 0;
        if (dest == UINT32_MAX)
        {
            cfg_error(cfg, tpath, "destination must be an IPv4 address (host names are not supported)");
            dest = 0;
        }
        if (ammio_publishes)
        {
            // Sent to the telegram's destination, or to the SUT's own address if none is given
            tel->dest_ip = dest ? dest : (sut_perspective ? out->host_ip : 0);
            if (tel->dest_ip == 0)
                cfg_error(cfg, tpath, "no destination address for a telegram ammio has to publish");
            if (tel->period_ms == 0)
                cfg_error(cfg, tpath, "no cycle time for a telegram ammio has to publish");
        }
        else
        {
            // Join the multicast group if the destination is one
            tel->dest_ip = (dest >> 28) == 0xE ? dest : 0;
            uint32_t src = x->srcCnt > 0 ? parse_host(x->pSrc[0].pUriHost1) : 0;
            tel->source_ip = src == UINT32_MAX ? 0 : src;
        }
        out->n_telegrams++;
    }

    tau_freeTelegrams(n_exchg, exchg);
    tau_freeXmlDatasetConfig(n_map, map, n_datasets, datasets);
    tau_freeXmlDoc(&doc);

    if (cfg->errors != errors_before)
    {
        trdp_xml_import_free(out);
        return -1;
    }
    return 0;
}

void trdp_xml_import_free(trdp_xml_import_t *import)
{
    for (size_t t = 0; t < import->n_telegrams; t++)
    {
        trdp_xml_telegram_t *tel = &import->telegrams[t];
        for (size_t k = 0; k < tel->n_vars; k++)
            free(tel->vars[k].var_id);
        free(tel->vars);
        free(tel->id);
    }
    free(import->telegrams);
    memset(import, 0, sizeof(*import));
}
