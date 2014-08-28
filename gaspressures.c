/*  gaspressures.c
 *  ---------------
 *  This file contains the routines to calculate the gas pressures in the cylinders.
 *  The functions below support the code in profile.c.
 *  The high-level function is populate_pressure_information(), called by function
 *  create_plot_info_new() in profile.c. The other functions below are, in turn,
 *  called by populate_pressure_information(). The calling sequence is as follows:
 *
 *  populate_pressure_information() -> calc_pressure_time()
 *                                  -> fill_missing_tank_pressures() -> fill_missing_segment_pressures()
 *                                                                   -> get_pr_interpolate_data()
 *
 *  The pr_track_t related functions below implement a linked list that is used by
 *  the majority of the functions below. The linked list covers a part of the dive profile
 *  for which there are no cylinder pressure data. Each element in the linked list
 *  represents a segment between two consecutive points on the dive profile.
 *  pr_track_t is defined in gaspressures.h
 */

#include "dive.h"
#include "display.h"
#include "profile.h"
#include "gaspressures.h"

static pr_track_t *pr_track_alloc(int start, int t_start)
{
	pr_track_t *pt = malloc(sizeof(pr_track_t));
	pt->start = start;
	pt->end = 0;
	pt->t_start = pt->t_end = t_start;
	pt->pressure_time = 0;
	pt->next = NULL;
	return pt;
}

/* poor man's linked list */
static pr_track_t *list_last(pr_track_t *list)
{
	pr_track_t *tail = list;
	if (!tail)
		return NULL;
	while (tail->next) {
		tail = tail->next;
	}
	return tail;
}

static pr_track_t *list_add(pr_track_t *list, pr_track_t *element)
{
	pr_track_t *tail = list_last(list);
	if (!tail)
		return element;
	tail->next = element;
	return list;
}

static void list_free(pr_track_t *list)
{
	if (!list)
		return;
	list_free(list->next);
	free(list);
}

#ifdef DEBUG_PR_TRACK
static void dump_pr_track(pr_track_t **track_pr)
{
	int cyl;
	pr_track_t *list;

	for (cyl = 0; cyl < MAX_CYLINDERS; cyl++) {
		list = track_pr[cyl];
		while (list) {
			printf("cyl%d: start %d end %d t_start %d t_end %d pt %d\n", cyl,
			       list->start, list->end, list->t_start, list->t_end, list->pressure_time);
			list = list->next;
		}
	}
}
#endif

/*
 * This looks at the pressures for one cylinder, and
 * calculates any missing beginning/end pressures for
 * each segment by taking the over-all SAC-rate into
 * account for that cylinder.
 *
 * NOTE! Many segments have full pressure information
 * (both beginning and ending pressure). But if we have
 * switched away from a cylinder, we will have the
 * beginning pressure for the first segment with a
 * missing end pressure. We may then have one or more
 * segments without beginning or end pressures, until
 * we finally have a segment with an end pressure.
 *
 * We want to spread out the pressure over these missing
 * segments according to how big of a time_pressure area
 * they have.
 */
static void fill_missing_segment_pressures(pr_track_t *list)
{
	while (list) {
		int start = list->start, end;
		pr_track_t *tmp = list;
		int pt_sum = 0, pt = 0;

		for (;;) {
			pt_sum += tmp->pressure_time;
			end = tmp->end;
			if (end)
				break;
			end = start;
			if (!tmp->next)
				break;
			tmp = tmp->next;
		}

		if (!start)
			start = end;

		/*
		 * Now 'start' and 'end' contain the pressure values
		 * for the set of segments described by 'list'..'tmp'.
		 * pt_sum is the sum of all the pressure-times of the
		 * segments.
		 *
		 * Now dole out the pressures relative to pressure-time.
		 */
		list->start = start;
		tmp->end = end;
		for (;;) {
			int pressure;
			pt += list->pressure_time;
			pressure = start;
			if (pt_sum)
				pressure -= (start - end) * (double)pt / pt_sum;
			list->end = pressure;
			if (list == tmp)
				break;
			list = list->next;
			list->start = pressure;
		}

		/* Ok, we've done that set of segments */
		list = list->next;
	}
}

#ifdef DEBUG_PR_INTERPOLATE
void dump_pr_interpolate(int i, pr_interpolate_t interpolate_pr)
{
	printf("Interpolate for entry %d: start %d - end %d - pt %d - acc_pt %d\n", i,
	       interpolate_pr.start, interpolate_pr.end, interpolate_pr.pressure_time, interpolate_pr.acc_pressure_time);
}
#endif


static struct pr_interpolate_struct get_pr_interpolate_data(pr_track_t *segment, struct plot_info *pi, int cur, int diluent_flag)
{ // cur = index to pi->entry corresponding to t_end of segment; diluent_flag=1 indicates diluent cylinder
	struct pr_interpolate_struct interpolate;
	int i;
	struct plot_data *entry;
	int pressure;

	interpolate.start = segment->start;
	interpolate.end = segment->end;
	interpolate.acc_pressure_time = 0;
	interpolate.pressure_time = 0;

	for (i = 0; i < pi->nr; i++) {
		entry = pi->entry + i;
		if (diluent_flag)
			pressure = DILUENT_PRESSURE(entry);
		else
			pressure = SENSOR_PRESSURE(entry);

		if (entry->sec < segment->t_start)
			continue;
		if (entry->sec >= segment->t_end) {
			interpolate.pressure_time += entry->pressure_time;
			break;
		}
		if (entry->sec == segment->t_start) {
			interpolate.acc_pressure_time = 0;
			interpolate.pressure_time = 0;
			if (pressure)
				interpolate.start = pressure;
			continue;
		}
		if (i < cur) {
			if (pressure) {
				interpolate.start = pressure;
				interpolate.acc_pressure_time = 0;
				interpolate.pressure_time = 0;
			} else {
				interpolate.acc_pressure_time += entry->pressure_time;
				interpolate.pressure_time += entry->pressure_time;
			}
			continue;
		}
		if (i == cur) {
			interpolate.acc_pressure_time += entry->pressure_time;
			interpolate.pressure_time += entry->pressure_time;
			continue;
		}
		interpolate.pressure_time += entry->pressure_time;
		if (pressure) {
			interpolate.end = pressure;
			break;
		}
	}
	return interpolate;
}

static void fill_missing_tank_pressures(struct dive *dive, struct plot_info *pi, pr_track_t **track_pr, int diluent_flag)
{
	int cyl, i;
	struct plot_data *entry;
	int cur_pr[MAX_CYLINDERS]; // cur_pr[MAX_CYLINDERS] is the CCR diluent cylinder

	for (cyl = 0; cyl < MAX_CYLINDERS; cyl++) {
		if (!track_pr[cyl]) {
			/* no segment where this cylinder is used */
			cur_pr[cyl] = -1;
			continue;
		}
		fill_missing_segment_pressures(track_pr[cyl]); // Interpolate the missing tank pressure values ..
		cur_pr[cyl] = track_pr[cyl]->start;	       // in the pr_track_t lists of structures
	}						       // and keep the starting pressure for each cylinder.

#ifdef DEBUG_PR_TRACK
	/* another great debugging tool */
	dump_pr_track(track_pr);
#endif

	/* Transfer interpolated cylinder pressures from pr_track strucktures to plotdata
	 * Go down the list of tank pressures in plot_info. Align them with the start &
	 * end times of each profile segment represented by a pr_track_t structure. Get
	 * the accumulated pressure_depths from the pr_track_t structures and then
	 * interpolate the pressure where these do not exist in the plot_info pressure
	 * variables. Pressure values are transferred from the pr_track_t structures
	 * to the plot_info structure, allowing us to plot the tank pressure.
	 *
	 * The first two pi structures are "fillers", but in case we don't have a sample
	 * at time 0 we need to process the second of them here, therefore i=1 */
	for (i = 1; i < pi->nr; i++) { // For each point on the profile:
		double magic;
		pr_track_t *segment;
		pr_interpolate_t interpolate;
		int pressure;
		int *save_pressure, *save_interpolated;

		entry = pi->entry + i;

		if (diluent_flag) {		// Find the cylinder index (cyl) ..
			cyl = DILUENT_CYLINDER; // .. as well as the cylinder pressure
			pressure = DILUENT_PRESSURE(entry);
			save_pressure = &(entry->diluentpressure[SENSOR_PR]);
			save_interpolated = &(entry->diluentpressure[INTERPOLATED_PR]);
		} else {
			pressure = SENSOR_PRESSURE(entry);
			save_pressure = &(entry->pressure[SENSOR_PR]);
			save_interpolated = &(entry->pressure[INTERPOLATED_PR]);
			cyl = entry->cylinderindex;
		}

		if (pressure) {			// If there is a valid pressure value,
			cur_pr[cyl] = pressure; // set current pressure
			continue;		// and skip to next point.
		}
		// If there is NO valid pressure value..
		// Find the pressure segment corresponding to this entry..
		segment = track_pr[cyl];
		while (segment && segment->t_end < entry->sec) // Find the track_pr with end time..
			segment = segment->next;	       // ..that matches the plot_info time (entry->sec)

		if (!segment || !segment->pressure_time) { // No (or empty) segment?
			*save_pressure = cur_pr[cyl];      // Just use our current pressure
			continue;			   // and skip to next point.
		}
		// If there is a valid segment but no tank pressure ..
		interpolate = get_pr_interpolate_data(segment, pi, i, diluent_flag); // Set up an interpolation structure

		/* if this segment has pressure_time, then calculate a new interpolated pressure */
		if (interpolate.pressure_time) {
			/* Overall pressure change over total pressure-time for this segment*/
			magic = (interpolate.end - interpolate.start) / (double)interpolate.pressure_time;

			/* Use that overall pressure change to update the current pressure */
			cur_pr[cyl] = rint(interpolate.start + magic * interpolate.acc_pressure_time);
		}
		*save_interpolated = cur_pr[cyl]; // and store the interpolated data in plot_info
	}
}


/*
 * What's the pressure-time between two plot data entries?
 * We're calculating the integral of pressure over time by
 * adding these up.
 *
 * The units won't matter as long as everybody agrees about
 * them, since they'll cancel out - we use this to calculate
 * a constant SAC-rate-equivalent, but we only use it to
 * scale pressures, so it ends up being a unitless scaling
 * factor.
 */
static inline int calc_pressure_time(struct dive *dive, struct divecomputer *dc, struct plot_data *a, struct plot_data *b)
{
	int time = b->sec - a->sec;
	int depth = (a->depth + b->depth) / 2;

	if (depth <= SURFACE_THRESHOLD)
		return 0;

	return depth_to_mbar(depth, dive) * time;
}

void populate_pressure_information(struct dive *dive, struct divecomputer *dc, struct plot_info *pi)
{
	int i, cylinderindex;
	pr_track_t *track_pr[MAX_CYLINDERS] = { NULL, };
	pr_track_t *current;
	bool missing_pr = false;

	cylinderindex = -1;
	current = NULL;
	for (i = 0; i < pi->nr; i++) {
		struct plot_data *entry = pi->entry + i;
		int pressure = SENSOR_PRESSURE(entry);

		/* discrete integration of pressure over time to get the SAC rate equivalent */
		if (current) {
			entry->pressure_time = calc_pressure_time(dive, dc, entry - 1, entry);
			current->pressure_time += entry->pressure_time;
			current->t_end = entry->sec;
		}

		/* track the segments per cylinder and their pressure/time integral */
		if (entry->cylinderindex != cylinderindex) {
			cylinderindex = entry->cylinderindex;
			current = pr_track_alloc(pressure, entry->sec);
			track_pr[cylinderindex] = list_add(track_pr[cylinderindex], current);
			continue;
		}

		if (!pressure) {
			missing_pr = 1;
			continue;
		}

		current->end = pressure;

		/* Was it continuous? */
		if (SENSOR_PRESSURE(entry - 1))
			continue;

		/* transmitter changed its working status */
		current = pr_track_alloc(pressure, entry->sec);
		track_pr[cylinderindex] = list_add(track_pr[cylinderindex], current);
	}

	if (missing_pr) {
		fill_missing_tank_pressures(dive, pi, track_pr, 0);
	}
	for (i = 0; i < MAX_CYLINDERS; i++)
		list_free(track_pr[i]);
}
