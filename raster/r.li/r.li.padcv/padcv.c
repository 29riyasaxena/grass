/****************************************************************************
 *
 * MODULE:       r.li.padcv
 * AUTHOR(S):    Serena Pallecchi (original contributor)
 *                student of Computer Science University of Pisa (Italy)
 *               Commission from Faunalia Pontedera (PI) www.faunalia.it
 *               Fixes: Markus Neteler <neteler itc.it>
 *               Rewrite: Markus Metz
 *               Patch identification: Michael Shapiro - CERL
 *
 * PURPOSE:      calculates coefficient of variation of patch areas
 * COPYRIGHT:    (C) 2007-2014 by the GRASS Development Team
 *
 *               This program is free software under the GNU General Public
 *               License (>=v2). Read the file COPYING that comes with GRASS
 *               for details.
 *
 *****************************************************************************/

#include <stdlib.h>
#include <fcntl.h>
#include <math.h>

#include <grass/gis.h>
#include <grass/raster.h>
#include <grass/glocale.h>

#include "../r.li.daemon/daemon.h"
#include "../r.li.daemon/GenericCell.h"

/* template is patchnum */

/* cell count and type of each patch */
struct pst {
    long count;
    generic_cell type;
};

rli_func patchAreaDistributionCV;
int calculate(int fd, struct area_entry *ad, double *result);
int calculateD(int fd, struct area_entry *ad, double *result);
int calculateF(int fd, struct area_entry *ad, double *result);

int main(int argc, char *argv[])
{
    struct Option *raster, *conf, *output;
    struct GModule *module;

    G_gisinit(argv[0]);
    module = G_define_module();
    module->description =
        _("Calculates coefficient of variation of patch area on a raster map");
    G_add_keyword(_("raster"));
    G_add_keyword(_("landscape structure analysis"));
    G_add_keyword(_("patch index"));

    /* define options */
    raster = G_define_standard_option(G_OPT_R_INPUT);

    conf = G_define_standard_option(G_OPT_F_INPUT);
    conf->key = "config";
    conf->description = _("Configuration file");
    conf->required = YES;

    output = G_define_standard_option(G_OPT_R_OUTPUT);

    if (G_parser(argc, argv))
        exit(EXIT_FAILURE);
    return calculateIndex(conf->answer, patchAreaDistributionCV, NULL,
                          raster->answer, output->answer);
}

int patchAreaDistributionCV(int fd, char **par UNUSED, struct area_entry *ad,
                            double *result)
{
    double indice = 0;
    int ris = RLI_OK;

    switch (ad->data_type) {
    case CELL_TYPE: {
        ris = calculate(fd, ad, &indice);
        break;
    }
    case DCELL_TYPE: {
        ris = calculateD(fd, ad, &indice);
        break;
    }
    case FCELL_TYPE: {
        ris = calculateF(fd, ad, &indice);
        break;
    }
    default: {
        G_fatal_error("data type unknown");
    }
    }

    if (ris != RLI_OK) {
        *result = -1;
        return RLI_ERRORE;
    }

    *result = indice;

    return RLI_OK;
}

int calculate(int fd, struct area_entry *ad, double *result)
{
    CELL *buf, *buf_sup, *buf_null;
    CELL corrCell, precCell, supCell;
    long npatch, area;
    long pid, old_pid, new_pid, *pid_corr, *pid_sup, *ltmp;
    struct pst *pst;
    long nalloc, incr;
    int i, j, k;
    int connected;
    int res = RLI_OK;
    int mask_fd, *mask_buf, *mask_sup, *mask_tmp, masked;
    struct Cell_head hd;

    Rast_get_window(&hd);

    buf_null = Rast_allocate_c_buf();
    Rast_set_c_null_value(buf_null, Rast_window_cols());
    buf_sup = buf_null;

    /* initialize patch ids */
    pid_corr = G_malloc(Rast_window_cols() * sizeof(long));
    pid_sup = G_malloc(Rast_window_cols() * sizeof(long));

    for (j = 0; j < Rast_window_cols(); j++) {
        pid_corr[j] = 0;
        pid_sup[j] = 0;
    }

    /* open mask if needed */
    mask_fd = -1;
    mask_buf = mask_sup = NULL;
    masked = FALSE;
    if (ad->mask == 1) {
        if ((mask_fd = open(ad->mask_name, O_RDONLY, 0755)) < 0) {
            G_free(buf_null);
            G_free(pid_corr);
            G_free(pid_sup);
            return RLI_ERRORE;
        }
        mask_buf = G_malloc(ad->cl * sizeof(int));
        if (mask_buf == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        mask_sup = G_malloc(ad->cl * sizeof(int));
        if (mask_sup == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        for (j = 0; j < ad->cl; j++)
            mask_buf[j] = 0;

        masked = TRUE;
    }

    /* calculate number of patches */
    npatch = 0;
    area = 0;
    pid = 0;

    /* patch size and type */
    incr = 1024;
    if (incr > ad->rl)
        incr = ad->rl;
    if (incr > ad->cl)
        incr = ad->cl;
    if (incr < 2)
        incr = 2;
    nalloc = incr;
    pst = G_malloc(nalloc * sizeof(struct pst));
    for (k = 0; k < nalloc; k++) {
        pst[k].count = 0;
    }

    for (i = 0; i < ad->rl; i++) {
        buf = RLI_get_cell_raster_row(fd, i + ad->y, ad);
        if (i > 0) {
            buf_sup = RLI_get_cell_raster_row(fd, i - 1 + ad->y, ad);
        }

        if (masked) {
            mask_tmp = mask_sup;
            mask_sup = mask_buf;
            mask_buf = mask_tmp;
            if (read(mask_fd, mask_buf, (ad->cl * sizeof(int))) < 0) {
                res = 0;
                goto free_exit;
            }
        }

        ltmp = pid_sup;
        pid_sup = pid_corr;
        pid_corr = ltmp;

        Rast_set_c_null_value(&precCell, 1);

        connected = 0;
        for (j = 0; j < ad->cl; j++) {
            pid_corr[j + ad->x] = 0;

            corrCell = buf[j + ad->x];
            if (masked && (mask_buf[j] == 0)) {
                Rast_set_c_null_value(&corrCell, 1);
            }

            if (Rast_is_c_null_value(&corrCell)) {
                connected = 0;
                precCell = corrCell;
                continue;
            }

            area++;

            supCell = buf_sup[j + ad->x];
            if (masked && (mask_sup[j] == 0)) {
                Rast_set_c_null_value(&supCell, 1);
            }

            if (!Rast_is_c_null_value(&precCell) && corrCell == precCell) {
                pid_corr[j + ad->x] = pid_corr[j - 1 + ad->x];
                connected = 1;
                pst[pid_corr[j + ad->x]].count++;
            }
            else {
                connected = 0;
            }

            if (!Rast_is_c_null_value(&supCell) && corrCell == supCell) {

                if (pid_corr[j + ad->x] != pid_sup[j + ad->x]) {
                    /* connect or merge */
                    /* after r.clump */
                    if (connected) {
                        npatch--;

                        if (npatch == 0) {
                            if (masked)
                                close(mask_fd);
                            G_fatal_error("npatch == 0 at row %d, col %d", i,
                                          j);
                        }
                    }

                    old_pid = pid_corr[j + ad->x];
                    new_pid = pid_sup[j + ad->x];
                    pid_corr[j + ad->x] = new_pid;
                    if (old_pid > 0) {
                        /* merge */
                        /* update left side of the current row */
                        for (k = 0; k < j; k++) {
                            if (pid_corr[k + ad->x] == old_pid)
                                pid_corr[k + ad->x] = new_pid;
                        }
                        /* update right side of the previous row */
                        for (k = j + 1; k < ad->cl; k++) {
                            if (pid_sup[k + ad->x] == old_pid)
                                pid_sup[k + ad->x] = new_pid;
                        }
                        pst[new_pid].count += pst[old_pid].count;
                        pst[old_pid].count = 0;

                        if (old_pid == pid)
                            pid--;
                    }
                    else {
                        pst[new_pid].count++;
                    }
                }
                connected = 1;
            }

            if (!connected) {
                /* start new patch */
                npatch++;
                pid++;
                pid_corr[j + ad->x] = pid;

                if (pid >= nalloc) {
                    pst = (struct pst *)G_realloc(pst, (pid + incr) *
                                                           sizeof(struct pst));

                    for (k = nalloc; k < pid + incr; k++)
                        pst[k].count = 0;

                    nalloc = pid + incr;
                }

                pst[pid].count = 1;
                pst[pid].type.t = CELL_TYPE;
                pst[pid].type.val.c = corrCell;
            }
            precCell = corrCell;
        }
    }

    if (npatch > 0) {
        double EW_DIST1, EW_DIST2, NS_DIST1, NS_DIST2;
        double area_units;
        double area_p;
        double cell_size_m;
        double mps;
        double psd;
        double pcv;
        double diff;

        /* calculate distance */
        G_begin_distance_calculations();
        /* EW Dist at North edge */
        EW_DIST1 = G_distance(hd.east, hd.north, hd.west, hd.north);
        /* EW Dist at South Edge */
        EW_DIST2 = G_distance(hd.east, hd.south, hd.west, hd.south);
        /* NS Dist at East edge */
        NS_DIST1 = G_distance(hd.east, hd.north, hd.east, hd.south);
        /* NS Dist at West edge */
        NS_DIST2 = G_distance(hd.west, hd.north, hd.west, hd.south);

        cell_size_m = (((EW_DIST1 + EW_DIST2) / 2) / hd.cols) *
                      (((NS_DIST1 + NS_DIST2) / 2) / hd.rows);
        area_units = cell_size_m * area;

        /* mean patch size in hectares */
        mps = area_units / (npatch * 10000);

        /* calculate patch size standard deviation */
        psd = 0;
        for (old_pid = 1; old_pid <= pid; old_pid++) {
            if (pst[old_pid].count > 0) {
                area_p = cell_size_m * pst[old_pid].count / 10000;
                diff = area_p - mps;
                psd += diff * diff;
            }
        }
        psd = sqrt(psd / npatch);
        pcv = psd * 100 / mps;
        *result = pcv;
    }
    else
        Rast_set_d_null_value(result, 1);

free_exit:
    if (masked) {
        close(mask_fd);
        G_free(mask_buf);
        G_free(mask_sup);
    }
    G_free(buf_null);
    G_free(pid_corr);
    G_free(pid_sup);
    G_free(pst);

    return res;
}

int calculateD(int fd, struct area_entry *ad, double *result)
{
    DCELL *buf, *buf_sup, *buf_null;
    DCELL corrCell, precCell, supCell;
    long npatch, area;
    long pid, old_pid, new_pid, *pid_corr, *pid_sup, *ltmp;
    struct pst *pst;
    long nalloc, incr;
    int i, j, k;
    int connected;
    int res = RLI_OK;
    int mask_fd, *mask_buf, *mask_sup, *mask_tmp, masked;
    struct Cell_head hd;

    Rast_get_window(&hd);

    buf_null = Rast_allocate_d_buf();
    Rast_set_d_null_value(buf_null, Rast_window_cols());
    buf_sup = buf_null;

    /* initialize patch ids */
    pid_corr = G_malloc(Rast_window_cols() * sizeof(long));
    pid_sup = G_malloc(Rast_window_cols() * sizeof(long));

    for (j = 0; j < Rast_window_cols(); j++) {
        pid_corr[j] = 0;
        pid_sup[j] = 0;
    }

    /* open mask if needed */
    mask_fd = -1;
    mask_buf = mask_sup = NULL;
    masked = FALSE;
    if (ad->mask == 1) {
        if ((mask_fd = open(ad->mask_name, O_RDONLY, 0755)) < 0) {
            G_free(buf_null);
            G_free(pid_corr);
            G_free(pid_sup);
            return RLI_ERRORE;
        }
        mask_buf = G_malloc(ad->cl * sizeof(int));
        if (mask_buf == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        mask_sup = G_malloc(ad->cl * sizeof(int));
        if (mask_sup == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        for (j = 0; j < ad->cl; j++)
            mask_buf[j] = 0;

        masked = TRUE;
    }

    /* calculate number of patches */
    npatch = 0;
    area = 0;
    pid = 0;

    /* patch size and type */
    incr = 1024;
    if (incr > ad->rl)
        incr = ad->rl;
    if (incr > ad->cl)
        incr = ad->cl;
    if (incr < 2)
        incr = 2;
    nalloc = incr;
    pst = G_malloc(nalloc * sizeof(struct pst));
    for (k = 0; k < nalloc; k++) {
        pst[k].count = 0;
    }

    for (i = 0; i < ad->rl; i++) {
        buf = RLI_get_dcell_raster_row(fd, i + ad->y, ad);
        if (i > 0) {
            buf_sup = RLI_get_dcell_raster_row(fd, i - 1 + ad->y, ad);
        }

        if (masked) {
            mask_tmp = mask_sup;
            mask_sup = mask_buf;
            mask_buf = mask_tmp;
            if (read(mask_fd, mask_buf, (ad->cl * sizeof(int))) < 0) {
                res = 0;
                goto free_exit;
            }
        }

        ltmp = pid_sup;
        pid_sup = pid_corr;
        pid_corr = ltmp;

        Rast_set_d_null_value(&precCell, 1);

        connected = 0;
        for (j = 0; j < ad->cl; j++) {
            pid_corr[j + ad->x] = 0;

            corrCell = buf[j + ad->x];
            if (masked && (mask_buf[j] == 0)) {
                Rast_set_d_null_value(&corrCell, 1);
            }

            if (Rast_is_d_null_value(&corrCell)) {
                connected = 0;
                precCell = corrCell;
                continue;
            }

            area++;

            supCell = buf_sup[j + ad->x];
            if (masked && (mask_sup[j] == 0)) {
                Rast_set_d_null_value(&supCell, 1);
            }

            if (!Rast_is_d_null_value(&precCell) && corrCell == precCell) {
                pid_corr[j + ad->x] = pid_corr[j - 1 + ad->x];
                connected = 1;
                pst[pid_corr[j + ad->x]].count++;
            }
            else {
                connected = 0;
            }

            if (!Rast_is_d_null_value(&supCell) && corrCell == supCell) {

                if (pid_corr[j + ad->x] != pid_sup[j + ad->x]) {
                    /* connect or merge */
                    /* after r.clump */
                    if (connected) {
                        npatch--;

                        if (npatch == 0) {
                            if (masked)
                                close(mask_fd);
                            G_fatal_error("npatch == 0 at row %d, col %d", i,
                                          j);
                        }
                    }

                    old_pid = pid_corr[j + ad->x];
                    new_pid = pid_sup[j + ad->x];
                    pid_corr[j + ad->x] = new_pid;
                    if (old_pid > 0) {
                        /* merge */
                        /* update left side of the current row */
                        for (k = 0; k < j; k++) {
                            if (pid_corr[k + ad->x] == old_pid)
                                pid_corr[k + ad->x] = new_pid;
                        }
                        /* update right side of the previous row */
                        for (k = j + 1; k < ad->cl; k++) {
                            if (pid_sup[k + ad->x] == old_pid)
                                pid_sup[k + ad->x] = new_pid;
                        }
                        pst[new_pid].count += pst[old_pid].count;
                        pst[old_pid].count = 0;

                        if (old_pid == pid)
                            pid--;
                    }
                    else {
                        pst[new_pid].count++;
                    }
                }
                connected = 1;
            }

            if (!connected) {
                /* start new patch */
                npatch++;
                pid++;
                pid_corr[j + ad->x] = pid;

                if (pid >= nalloc) {
                    pst = (struct pst *)G_realloc(pst, (pid + incr) *
                                                           sizeof(struct pst));

                    for (k = nalloc; k < pid + incr; k++)
                        pst[k].count = 0;

                    nalloc = pid + incr;
                }

                pst[pid].count = 1;
                pst[pid].type.t = CELL_TYPE;
                pst[pid].type.val.c = corrCell;
            }
            precCell = corrCell;
        }
    }

    if (npatch > 0) {
        double EW_DIST1, EW_DIST2, NS_DIST1, NS_DIST2;
        double area_units;
        double area_p;
        double cell_size_m;
        double mps;
        double psd;
        double pcv;
        double diff;

        /* calculate distance */
        G_begin_distance_calculations();
        /* EW Dist at North edge */
        EW_DIST1 = G_distance(hd.east, hd.north, hd.west, hd.north);
        /* EW Dist at South Edge */
        EW_DIST2 = G_distance(hd.east, hd.south, hd.west, hd.south);
        /* NS Dist at East edge */
        NS_DIST1 = G_distance(hd.east, hd.north, hd.east, hd.south);
        /* NS Dist at West edge */
        NS_DIST2 = G_distance(hd.west, hd.north, hd.west, hd.south);

        cell_size_m = (((EW_DIST1 + EW_DIST2) / 2) / hd.cols) *
                      (((NS_DIST1 + NS_DIST2) / 2) / hd.rows);
        area_units = cell_size_m * area;

        /* mean patch size in hectares */
        mps = area_units / (npatch * 10000);

        /* calculate patch size standard deviation */
        psd = 0;
        for (old_pid = 1; old_pid <= pid; old_pid++) {
            if (pst[old_pid].count > 0) {
                area_p = cell_size_m * pst[old_pid].count / 10000;
                diff = area_p - mps;
                psd += diff * diff;
            }
        }
        psd = sqrt(psd / npatch);
        pcv = psd * 100 / mps;
        *result = pcv;
    }
    else
        Rast_set_d_null_value(result, 1);

free_exit:
    if (masked) {
        close(mask_fd);
        G_free(mask_buf);
        G_free(mask_sup);
    }
    G_free(buf_null);
    G_free(pid_corr);
    G_free(pid_sup);
    G_free(pst);

    return res;
}

int calculateF(int fd, struct area_entry *ad, double *result)
{
    FCELL *buf, *buf_sup, *buf_null;
    FCELL corrCell, precCell, supCell;
    long npatch, area;
    long pid, old_pid, new_pid, *pid_corr, *pid_sup, *ltmp;
    struct pst *pst;
    long nalloc, incr;
    int i, j, k;
    int connected;
    int res = RLI_OK;
    int mask_fd, *mask_buf, *mask_sup, *mask_tmp, masked;
    struct Cell_head hd;

    Rast_get_window(&hd);

    buf_null = Rast_allocate_f_buf();
    Rast_set_f_null_value(buf_null, Rast_window_cols());
    buf_sup = buf_null;

    /* initialize patch ids */
    pid_corr = G_malloc(Rast_window_cols() * sizeof(long));
    pid_sup = G_malloc(Rast_window_cols() * sizeof(long));

    for (j = 0; j < Rast_window_cols(); j++) {
        pid_corr[j] = 0;
        pid_sup[j] = 0;
    }

    /* open mask if needed */
    mask_fd = -1;
    mask_buf = mask_sup = NULL;
    masked = FALSE;
    if (ad->mask == 1) {
        if ((mask_fd = open(ad->mask_name, O_RDONLY, 0755)) < 0) {
            G_free(buf_null);
            G_free(pid_corr);
            G_free(pid_sup);
            return RLI_ERRORE;
        }
        mask_buf = G_malloc(ad->cl * sizeof(int));
        if (mask_buf == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        mask_sup = G_malloc(ad->cl * sizeof(int));
        if (mask_sup == NULL) {
            close(mask_fd);
            G_fatal_error("malloc mask_buf failed");
        }
        for (j = 0; j < ad->cl; j++)
            mask_buf[j] = 0;

        masked = TRUE;
    }

    /* calculate number of patches */
    npatch = 0;
    area = 0;
    pid = 0;

    /* patch size and type */
    incr = 1024;
    if (incr > ad->rl)
        incr = ad->rl;
    if (incr > ad->cl)
        incr = ad->cl;
    if (incr < 2)
        incr = 2;
    nalloc = incr;
    pst = G_malloc(nalloc * sizeof(struct pst));
    for (k = 0; k < nalloc; k++) {
        pst[k].count = 0;
    }

    for (i = 0; i < ad->rl; i++) {
        buf = RLI_get_fcell_raster_row(fd, i + ad->y, ad);
        if (i > 0) {
            buf_sup = RLI_get_fcell_raster_row(fd, i - 1 + ad->y, ad);
        }

        if (masked) {
            mask_tmp = mask_sup;
            mask_sup = mask_buf;
            mask_buf = mask_tmp;
            if (read(mask_fd, mask_buf, (ad->cl * sizeof(int))) < 0) {
                res = 0;
                goto free_exit;
            }
        }

        ltmp = pid_sup;
        pid_sup = pid_corr;
        pid_corr = ltmp;

        Rast_set_f_null_value(&precCell, 1);

        connected = 0;
        for (j = 0; j < ad->cl; j++) {
            pid_corr[j + ad->x] = 0;

            corrCell = buf[j + ad->x];
            if (masked && (mask_buf[j] == 0)) {
                Rast_set_f_null_value(&corrCell, 1);
            }

            if (Rast_is_f_null_value(&corrCell)) {
                connected = 0;
                precCell = corrCell;
                continue;
            }

            area++;

            supCell = buf_sup[j + ad->x];
            if (masked && (mask_sup[j] == 0)) {
                Rast_set_f_null_value(&supCell, 1);
            }

            if (!Rast_is_f_null_value(&precCell) && corrCell == precCell) {
                pid_corr[j + ad->x] = pid_corr[j - 1 + ad->x];
                connected = 1;
                pst[pid_corr[j + ad->x]].count++;
            }
            else {
                connected = 0;
            }

            if (!Rast_is_f_null_value(&supCell) && corrCell == supCell) {

                if (pid_corr[j + ad->x] != pid_sup[j + ad->x]) {
                    /* connect or merge */
                    /* after r.clump */
                    if (connected) {
                        npatch--;

                        if (npatch == 0) {
                            if (masked)
                                close(mask_fd);
                            G_fatal_error("npatch == 0 at row %d, col %d", i,
                                          j);
                        }
                    }

                    old_pid = pid_corr[j + ad->x];
                    new_pid = pid_sup[j + ad->x];
                    pid_corr[j + ad->x] = new_pid;
                    if (old_pid > 0) {
                        /* merge */
                        /* update left side of the current row */
                        for (k = 0; k < j; k++) {
                            if (pid_corr[k + ad->x] == old_pid)
                                pid_corr[k + ad->x] = new_pid;
                        }
                        /* update right side of the previous row */
                        for (k = j + 1; k < ad->cl; k++) {
                            if (pid_sup[k + ad->x] == old_pid)
                                pid_sup[k + ad->x] = new_pid;
                        }
                        pst[new_pid].count += pst[old_pid].count;
                        pst[old_pid].count = 0;

                        if (old_pid == pid)
                            pid--;
                    }
                    else {
                        pst[new_pid].count++;
                    }
                }
                connected = 1;
            }

            if (!connected) {
                /* start new patch */
                npatch++;
                pid++;
                pid_corr[j + ad->x] = pid;

                if (pid >= nalloc) {
                    pst = (struct pst *)G_realloc(pst, (pid + incr) *
                                                           sizeof(struct pst));

                    for (k = nalloc; k < pid + incr; k++)
                        pst[k].count = 0;

                    nalloc = pid + incr;
                }

                pst[pid].count = 1;
                pst[pid].type.t = CELL_TYPE;
                pst[pid].type.val.c = corrCell;
            }
            precCell = corrCell;
        }
    }

    if (npatch > 0) {
        double EW_DIST1, EW_DIST2, NS_DIST1, NS_DIST2;
        double area_units;
        double area_p;
        double cell_size_m;
        double mps;
        double psd;
        double pcv;
        double diff;

        /* calculate distance */
        G_begin_distance_calculations();
        /* EW Dist at North edge */
        EW_DIST1 = G_distance(hd.east, hd.north, hd.west, hd.north);
        /* EW Dist at South Edge */
        EW_DIST2 = G_distance(hd.east, hd.south, hd.west, hd.south);
        /* NS Dist at East edge */
        NS_DIST1 = G_distance(hd.east, hd.north, hd.east, hd.south);
        /* NS Dist at West edge */
        NS_DIST2 = G_distance(hd.west, hd.north, hd.west, hd.south);

        cell_size_m = (((EW_DIST1 + EW_DIST2) / 2) / hd.cols) *
                      (((NS_DIST1 + NS_DIST2) / 2) / hd.rows);
        area_units = cell_size_m * area;

        /* mean patch size in hectares */
        mps = area_units / (npatch * 10000);

        /* calculate patch size standard deviation */
        psd = 0;
        for (old_pid = 1; old_pid <= pid; old_pid++) {
            if (pst[old_pid].count > 0) {
                area_p = cell_size_m * pst[old_pid].count / 10000;
                diff = area_p - mps;
                psd += diff * diff;
            }
        }
        psd = sqrt(psd / npatch);
        pcv = psd * 100 / mps;
        *result = pcv;
    }
    else
        Rast_set_d_null_value(result, 1);

free_exit:
    if (masked) {
        close(mask_fd);
        G_free(mask_buf);
        G_free(mask_sup);
    }
    G_free(buf_null);
    G_free(pid_corr);
    G_free(pid_sup);
    G_free(pst);

    return res;
}
