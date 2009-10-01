#include "isl_map_private.h"
#include "isl_seq.h"
#include "isl_tab.h"

/*
 * The implementation of parametric integer linear programming in this file
 * was inspired by the paper "Parametric Integer Programming" and the
 * report "Solving systems of affine (in)equalities" by Paul Feautrier
 * (and others).
 *
 * The strategy used for obtaining a feasible solution is different
 * from the one used in isl_tab.c.  In particular, in isl_tab.c,
 * upon finding a constraint that is not yet satisfied, we pivot
 * in a row that increases the constant term of row holding the
 * constraint, making sure the sample solution remains feasible
 * for all the constraints it already satisfied.
 * Here, we always pivot in the row holding the constraint,
 * choosing a column that induces the lexicographically smallest
 * increment to the sample solution.
 *
 * By starting out from a sample value that is lexicographically
 * smaller than any integer point in the problem space, the first
 * feasible integer sample point we find will also be the lexicographically
 * smallest.  If all variables can be assumed to be non-negative,
 * then the initial sample value may be chosen equal to zero.
 * However, we will not make this assumption.  Instead, we apply
 * the "big parameter" trick.  Any variable x is then not directly
 * used in the tableau, but instead it its represented by another
 * variable x' = M + x, where M is an arbitrarily large (positive)
 * value.  x' is therefore always non-negative, whatever the value of x.
 * Taking as initial smaple value x' = 0 corresponds to x = -M,
 * which is always smaller than any possible value of x.
 *
 * We use the big parameter trick both in the main tableau and
 * the context tableau, each of course having its own big parameter.
 * Before doing any real work, we check if all the parameters
 * happen to be non-negative.  If so, we drop the column corresponding
 * to M from the initial context tableau.
 */

/* isl_sol is an interface for constructing a solution to
 * a parametric integer linear programming problem.
 * Every time the algorithm reaches a state where a solution
 * can be read off from the tableau (including cases where the tableau
 * is empty), the function "add" is called on the isl_sol passed
 * to find_solutions_main.
 *
 * The context tableau is owned by isl_sol and is updated incrementally.
 *
 * There are currently two implementations of this interface,
 * isl_sol_map, which simply collects the solutions in an isl_map
 * and (optionally) the parts of the context where there is no solution
 * in an isl_set, and
 * isl_sol_for, which calls a user-defined function for each part of
 * the solution.
 */
struct isl_sol {
	struct isl_tab *context_tab;
	struct isl_sol *(*add)(struct isl_sol *sol, struct isl_tab *tab);
	void (*free)(struct isl_sol *sol);
};

static void sol_free(struct isl_sol *sol)
{
	if (!sol)
		return;
	sol->free(sol);
}

struct isl_sol_map {
	struct isl_sol	sol;
	struct isl_map	*map;
	struct isl_set	*empty;
	int		max;
};

static void sol_map_free(struct isl_sol_map *sol_map)
{
	isl_tab_free(sol_map->sol.context_tab);
	isl_map_free(sol_map->map);
	isl_set_free(sol_map->empty);
	free(sol_map);
}

static void sol_map_free_wrap(struct isl_sol *sol)
{
	sol_map_free((struct isl_sol_map *)sol);
}

static struct isl_sol_map *add_empty(struct isl_sol_map *sol)
{
	struct isl_basic_set *bset;

	if (!sol->empty)
		return sol;
	sol->empty = isl_set_grow(sol->empty, 1);
	bset = isl_basic_set_copy(sol->sol.context_tab->bset);
	bset = isl_basic_set_simplify(bset);
	bset = isl_basic_set_finalize(bset);
	sol->empty = isl_set_add(sol->empty, bset);
	if (!sol->empty)
		goto error;
	return sol;
error:
	sol_map_free(sol);
	return NULL;
}

/* Add the solution identified by the tableau and the context tableau.
 *
 * The layout of the variables is as follows.
 *	tab->n_var is equal to the total number of variables in the input
 *			map (including divs that were copied from the context)
 *			+ the number of extra divs constructed
 *      Of these, the first tab->n_param and the last tab->n_div variables
 *	correspond to the variables in the context, i.e.,
 *		tab->n_param + tab->n_div = context_tab->n_var
 *	tab->n_param is equal to the number of parameters and input
 *			dimensions in the input map
 *	tab->n_div is equal to the number of divs in the context
 *
 * If there is no solution, then the basic set corresponding to the
 * context tableau is added to the set "empty".
 *
 * Otherwise, a basic map is constructed with the same parameters
 * and divs as the context, the dimensions of the context as input
 * dimensions and a number of output dimensions that is equal to
 * the number of output dimensions in the input map.
 * The divs in the input map (if any) that do not correspond to any
 * div in the context do not appear in the solution.
 * The algorithm will make sure that they have an integer value,
 * but these values themselves are of no interest.
 *
 * The constraints and divs of the context are simply copied
 * fron context_tab->bset.
 * To extract the value of the output variables, it should be noted
 * that we always use a big parameter M and so the variable stored
 * in the tableau is not an output variable x itself, but
 *	x' = M + x (in case of minimization)
 * or
 *	x' = M - x (in case of maximization)
 * If x' appears in a column, then its optimal value is zero,
 * which means that the optimal value of x is an unbounded number
 * (-M for minimization and M for maximization).
 * We currently assume that the output dimensions in the original map
 * are bounded, so this cannot occur.
 * Similarly, when x' appears in a row, then the coefficient of M in that
 * row is necessarily 1.
 * If the row represents
 *	d x' = c + d M + e(y)
 * then, in case of minimization, an equality
 *	c + e(y) - d x' = 0
 * is added, and in case of maximization,
 *	c + e(y) + d x' = 0
 */
static struct isl_sol_map *sol_map_add(struct isl_sol_map *sol,
	struct isl_tab *tab)
{
	int i;
	struct isl_basic_map *bmap = NULL;
	struct isl_tab *context_tab;
	unsigned n_eq;
	unsigned n_ineq;
	unsigned nparam;
	unsigned total;
	unsigned n_div;
	unsigned n_out;
	unsigned off;

	if (!sol || !tab)
		goto error;

	if (tab->empty)
		return add_empty(sol);

	context_tab = sol->sol.context_tab;
	off = 2 + tab->M;
	n_out = isl_map_dim(sol->map, isl_dim_out);
	n_eq = context_tab->bset->n_eq + n_out;
	n_ineq = context_tab->bset->n_ineq;
	nparam = tab->n_param;
	total = isl_map_dim(sol->map, isl_dim_all);
	bmap = isl_basic_map_alloc_dim(isl_map_get_dim(sol->map),
				    tab->n_div, n_eq, 2 * tab->n_div + n_ineq);
	if (!bmap)
		goto error;
	n_div = tab->n_div;
	if (tab->rational)
		ISL_F_SET(bmap, ISL_BASIC_MAP_RATIONAL);
	for (i = 0; i < context_tab->bset->n_div; ++i) {
		int k = isl_basic_map_alloc_div(bmap);
		if (k < 0)
			goto error;
		isl_seq_cpy(bmap->div[k],
			    context_tab->bset->div[i], 1 + 1 + nparam);
		isl_seq_clr(bmap->div[k] + 1 + 1 + nparam, total - nparam);
		isl_seq_cpy(bmap->div[k] + 1 + 1 + total,
			    context_tab->bset->div[i] + 1 + 1 + nparam, i);
	}
	for (i = 0; i < context_tab->bset->n_eq; ++i) {
		int k = isl_basic_map_alloc_equality(bmap);
		if (k < 0)
			goto error;
		isl_seq_cpy(bmap->eq[k], context_tab->bset->eq[i], 1 + nparam);
		isl_seq_clr(bmap->eq[k] + 1 + nparam, total - nparam);
		isl_seq_cpy(bmap->eq[k] + 1 + total,
			    context_tab->bset->eq[i] + 1 + nparam, n_div);
	}
	for (i = 0; i < context_tab->bset->n_ineq; ++i) {
		int k = isl_basic_map_alloc_inequality(bmap);
		if (k < 0)
			goto error;
		isl_seq_cpy(bmap->ineq[k],
			context_tab->bset->ineq[i], 1 + nparam);
		isl_seq_clr(bmap->ineq[k] + 1 + nparam, total - nparam);
		isl_seq_cpy(bmap->ineq[k] + 1 + total,
			context_tab->bset->ineq[i] + 1 + nparam, n_div);
	}
	for (i = tab->n_param; i < total; ++i) {
		int k = isl_basic_map_alloc_equality(bmap);
		if (k < 0)
			goto error;
		isl_seq_clr(bmap->eq[k] + 1, isl_basic_map_total_dim(bmap));
		if (!tab->var[i].is_row) {
			/* no unbounded */
			isl_assert(bmap->ctx, !tab->M, goto error);
			isl_int_set_si(bmap->eq[k][0], 0);
			if (sol->max)
				isl_int_set_si(bmap->eq[k][1 + i], 1);
			else
				isl_int_set_si(bmap->eq[k][1 + i], -1);
		} else {
			int row, j;
			row = tab->var[i].index;
			/* no unbounded */
			if (tab->M)
				isl_assert(bmap->ctx,
					isl_int_eq(tab->mat->row[row][2],
						   tab->mat->row[row][0]),
					goto error);
			isl_int_set(bmap->eq[k][0], tab->mat->row[row][1]);
			for (j = 0; j < tab->n_param; ++j) {
				int col;
				if (tab->var[j].is_row)
					continue;
				col = tab->var[j].index;
				isl_int_set(bmap->eq[k][1 + j],
					    tab->mat->row[row][off + col]);
			}
			for (j = 0; j < tab->n_div; ++j) {
				int col;
				if (tab->var[tab->n_var - tab->n_div+j].is_row)
					continue;
				col = tab->var[tab->n_var - tab->n_div+j].index;
				isl_int_set(bmap->eq[k][1 + total + j],
					    tab->mat->row[row][off + col]);
			}
			if (sol->max)
				isl_int_set(bmap->eq[k][1 + i],
					    tab->mat->row[row][0]);
			else
				isl_int_neg(bmap->eq[k][1 + i],
					    tab->mat->row[row][0]);
		}
	}
	bmap = isl_basic_map_gauss(bmap, NULL);
	bmap = isl_basic_map_normalize_constraints(bmap);
	bmap = isl_basic_map_finalize(bmap);
	sol->map = isl_map_grow(sol->map, 1);
	sol->map = isl_map_add(sol->map, bmap);
	if (!sol->map)
		goto error;
	return sol;
error:
	isl_basic_map_free(bmap);
	sol_free(&sol->sol);
	return NULL;
}

static struct isl_sol *sol_map_add_wrap(struct isl_sol *sol,
	struct isl_tab *tab)
{
	return (struct isl_sol *)sol_map_add((struct isl_sol_map *)sol, tab);
}


static struct isl_basic_set *isl_basic_set_add_ineq(struct isl_basic_set *bset,
	isl_int *ineq)
{
	int k;

	bset = isl_basic_set_extend_constraints(bset, 0, 1);
	if (!bset)
		return NULL;
	k = isl_basic_set_alloc_inequality(bset);
	if (k < 0)
		goto error;
	isl_seq_cpy(bset->ineq[k], ineq, 1 + isl_basic_set_total_dim(bset));
	return bset;
error:
	isl_basic_set_free(bset);
	return NULL;
}

static struct isl_basic_set *isl_basic_set_add_eq(struct isl_basic_set *bset,
	isl_int *eq)
{
	int k;

	bset = isl_basic_set_extend_constraints(bset, 1, 0);
	if (!bset)
		return NULL;
	k = isl_basic_set_alloc_equality(bset);
	if (k < 0)
		goto error;
	isl_seq_cpy(bset->eq[k], eq, 1 + isl_basic_set_total_dim(bset));
	return bset;
error:
	isl_basic_set_free(bset);
	return NULL;
}


/* Store the "parametric constant" of row "row" of tableau "tab" in "line",
 * i.e., the constant term and the coefficients of all variables that
 * appear in the context tableau.
 * Note that the coefficient of the big parameter M is NOT copied.
 * The context tableau may not have a big parameter and even when it
 * does, it is a different big parameter.
 */
static void get_row_parameter_line(struct isl_tab *tab, int row, isl_int *line)
{
	int i;
	unsigned off = 2 + tab->M;

	isl_int_set(line[0], tab->mat->row[row][1]);
	for (i = 0; i < tab->n_param; ++i) {
		if (tab->var[i].is_row)
			isl_int_set_si(line[1 + i], 0);
		else {
			int col = tab->var[i].index;
			isl_int_set(line[1 + i], tab->mat->row[row][off + col]);
		}
	}
	for (i = 0; i < tab->n_div; ++i) {
		if (tab->var[tab->n_var - tab->n_div + i].is_row)
			isl_int_set_si(line[1 + tab->n_param + i], 0);
		else {
			int col = tab->var[tab->n_var - tab->n_div + i].index;
			isl_int_set(line[1 + tab->n_param + i],
				    tab->mat->row[row][off + col]);
		}
	}
}

/* Check if rows "row1" and "row2" have identical "parametric constants",
 * as explained above.
 * In this case, we also insist that the coefficients of the big parameter
 * be the same as the values of the constants will only be the same
 * if these coefficients are also the same.
 */
static int identical_parameter_line(struct isl_tab *tab, int row1, int row2)
{
	int i;
	unsigned off = 2 + tab->M;

	if (isl_int_ne(tab->mat->row[row1][1], tab->mat->row[row2][1]))
		return 0;

	if (tab->M && isl_int_ne(tab->mat->row[row1][2],
				 tab->mat->row[row2][2]))
		return 0;

	for (i = 0; i < tab->n_param + tab->n_div; ++i) {
		int pos = i < tab->n_param ? i :
			tab->n_var - tab->n_div + i - tab->n_param;
		int col;

		if (tab->var[pos].is_row)
			continue;
		col = tab->var[pos].index;
		if (isl_int_ne(tab->mat->row[row1][off + col],
			       tab->mat->row[row2][off + col]))
			return 0;
	}
	return 1;
}

/* Return an inequality that expresses that the "parametric constant"
 * should be non-negative.
 * This function is only called when the coefficient of the big parameter
 * is equal to zero.
 */
static struct isl_vec *get_row_parameter_ineq(struct isl_tab *tab, int row)
{
	struct isl_vec *ineq;

	ineq = isl_vec_alloc(tab->mat->ctx, 1 + tab->n_param + tab->n_div);
	if (!ineq)
		return NULL;

	get_row_parameter_line(tab, row, ineq->el);
	if (ineq)
		ineq = isl_vec_normalize(ineq);

	return ineq;
}

/* Return a integer division for use in a parametric cut based on the given row.
 * In particular, let the parametric constant of the row be
 *
 *		\sum_i a_i y_i
 *
 * where y_0 = 1, but none of the y_i corresponds to the big parameter M.
 * The div returned is equal to
 *
 *		floor(\sum_i {-a_i} y_i) = floor((\sum_i (-a_i mod d) y_i)/d)
 */
static struct isl_vec *get_row_parameter_div(struct isl_tab *tab, int row)
{
	struct isl_vec *div;

	div = isl_vec_alloc(tab->mat->ctx, 1 + 1 + tab->n_param + tab->n_div);
	if (!div)
		return NULL;

	isl_int_set(div->el[0], tab->mat->row[row][0]);
	get_row_parameter_line(tab, row, div->el + 1);
	div = isl_vec_normalize(div);
	isl_seq_neg(div->el + 1, div->el + 1, div->size - 1);
	isl_seq_fdiv_r(div->el + 1, div->el + 1, div->el[0], div->size - 1);

	return div;
}

/* Return a integer division for use in transferring an integrality constraint
 * to the context.
 * In particular, let the parametric constant of the row be
 *
 *		\sum_i a_i y_i
 *
 * where y_0 = 1, but none of the y_i corresponds to the big parameter M.
 * The the returned div is equal to
 *
 *		floor(\sum_i {a_i} y_i) = floor((\sum_i (a_i mod d) y_i)/d)
 */
static struct isl_vec *get_row_split_div(struct isl_tab *tab, int row)
{
	struct isl_vec *div;

	div = isl_vec_alloc(tab->mat->ctx, 1 + 1 + tab->n_param + tab->n_div);
	if (!div)
		return NULL;

	isl_int_set(div->el[0], tab->mat->row[row][0]);
	get_row_parameter_line(tab, row, div->el + 1);
	div = isl_vec_normalize(div);
	isl_seq_fdiv_r(div->el + 1, div->el + 1, div->el[0], div->size - 1);

	return div;
}

/* Construct and return an inequality that expresses an upper bound
 * on the given div.
 * In particular, if the div is given by
 *
 *	d = floor(e/m)
 *
 * then the inequality expresses
 *
 *	m d <= e
 */
static struct isl_vec *ineq_for_div(struct isl_basic_set *bset, unsigned div)
{
	unsigned total;
	unsigned div_pos;
	struct isl_vec *ineq;

	total = isl_basic_set_total_dim(bset);
	div_pos = 1 + total - bset->n_div + div;

	ineq = isl_vec_alloc(bset->ctx, 1 + total);
	if (!ineq)
		return NULL;

	isl_seq_cpy(ineq->el, bset->div[div] + 1, 1 + total);
	isl_int_neg(ineq->el[div_pos], bset->div[div][0]);
	return ineq;
}

/* Given a row in the tableau and a div that was created
 * using get_row_split_div and that been constrained to equality, i.e.,
 *
 *		d = floor(\sum_i {a_i} y_i) = \sum_i {a_i} y_i
 *
 * replace the expression "\sum_i {a_i} y_i" in the row by d,
 * i.e., we subtract "\sum_i {a_i} y_i" and add 1 d.
 * The coefficients of the non-parameters in the tableau have been
 * verified to be integral.  We can therefore simply replace coefficient b
 * by floor(b).  For the coefficients of the parameters we have
 * floor(a_i) = a_i - {a_i}, while for the other coefficients, we have
 * floor(b) = b.
 */
static struct isl_tab *set_row_cst_to_div(struct isl_tab *tab, int row, int div)
{
	int col;
	unsigned off = 2 + tab->M;

	isl_seq_fdiv_q(tab->mat->row[row] + 1, tab->mat->row[row] + 1,
			tab->mat->row[row][0], 1 + tab->M + tab->n_col);

	isl_int_set_si(tab->mat->row[row][0], 1);

	isl_assert(tab->mat->ctx,
		!tab->var[tab->n_var - tab->n_div + div].is_row, goto error);

	col = tab->var[tab->n_var - tab->n_div + div].index;
	isl_int_set_si(tab->mat->row[row][off + col], 1);

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Check if the (parametric) constant of the given row is obviously
 * negative, meaning that we don't need to consult the context tableau.
 * If there is a big parameter and its coefficient is non-zero,
 * then this coefficient determines the outcome.
 * Otherwise, we check whether the constant is negative and
 * all non-zero coefficients of parameters are negative and
 * belong to non-negative parameters.
 */
static int is_obviously_neg(struct isl_tab *tab, int row)
{
	int i;
	int col;
	unsigned off = 2 + tab->M;

	if (tab->M) {
		if (isl_int_is_pos(tab->mat->row[row][2]))
			return 0;
		if (isl_int_is_neg(tab->mat->row[row][2]))
			return 1;
	}

	if (isl_int_is_nonneg(tab->mat->row[row][1]))
		return 0;
	for (i = 0; i < tab->n_param; ++i) {
		/* Eliminated parameter */
		if (tab->var[i].is_row)
			continue;
		col = tab->var[i].index;
		if (isl_int_is_zero(tab->mat->row[row][off + col]))
			continue;
		if (!tab->var[i].is_nonneg)
			return 0;
		if (isl_int_is_pos(tab->mat->row[row][off + col]))
			return 0;
	}
	for (i = 0; i < tab->n_div; ++i) {
		if (tab->var[tab->n_var - tab->n_div + i].is_row)
			continue;
		col = tab->var[tab->n_var - tab->n_div + i].index;
		if (isl_int_is_zero(tab->mat->row[row][off + col]))
			continue;
		if (!tab->var[tab->n_var - tab->n_div + i].is_nonneg)
			return 0;
		if (isl_int_is_pos(tab->mat->row[row][off + col]))
			return 0;
	}
	return 1;
}

/* Check if the (parametric) constant of the given row is obviously
 * non-negative, meaning that we don't need to consult the context tableau.
 * If there is a big parameter and its coefficient is non-zero,
 * then this coefficient determines the outcome.
 * Otherwise, we check whether the constant is non-negative and
 * all non-zero coefficients of parameters are positive and
 * belong to non-negative parameters.
 */
static int is_obviously_nonneg(struct isl_tab *tab, int row)
{
	int i;
	int col;
	unsigned off = 2 + tab->M;

	if (tab->M) {
		if (isl_int_is_pos(tab->mat->row[row][2]))
			return 1;
		if (isl_int_is_neg(tab->mat->row[row][2]))
			return 0;
	}

	if (isl_int_is_neg(tab->mat->row[row][1]))
		return 0;
	for (i = 0; i < tab->n_param; ++i) {
		/* Eliminated parameter */
		if (tab->var[i].is_row)
			continue;
		col = tab->var[i].index;
		if (isl_int_is_zero(tab->mat->row[row][off + col]))
			continue;
		if (!tab->var[i].is_nonneg)
			return 0;
		if (isl_int_is_neg(tab->mat->row[row][off + col]))
			return 0;
	}
	for (i = 0; i < tab->n_div; ++i) {
		if (tab->var[tab->n_var - tab->n_div + i].is_row)
			continue;
		col = tab->var[tab->n_var - tab->n_div + i].index;
		if (isl_int_is_zero(tab->mat->row[row][off + col]))
			continue;
		if (!tab->var[tab->n_var - tab->n_div + i].is_nonneg)
			return 0;
		if (isl_int_is_neg(tab->mat->row[row][off + col]))
			return 0;
	}
	return 1;
}

/* Given a row r and two columns, return the column that would
 * lead to the lexicographically smallest increment in the sample
 * solution when leaving the basis in favor of the row.
 * Pivoting with column c will increment the sample value by a non-negative
 * constant times a_{V,c}/a_{r,c}, with a_{V,c} the elements of column c
 * corresponding to the non-parametric variables.
 * If variable v appears in a column c_v, the a_{v,c} = 1 iff c = c_v,
 * with all other entries in this virtual row equal to zero.
 * If variable v appears in a row, then a_{v,c} is the element in column c
 * of that row.
 *
 * Let v be the first variable with a_{v,c1}/a_{r,c1} != a_{v,c2}/a_{r,c2}.
 * Then if a_{v,c1}/a_{r,c1} < a_{v,c2}/a_{r,c2}, i.e.,
 * a_{v,c2} a_{r,c1} - a_{v,c1} a_{r,c2} > 0, c1 results in the minimal
 * increment.  Otherwise, it's c2.
 */
static int lexmin_col_pair(struct isl_tab *tab,
	int row, int col1, int col2, isl_int tmp)
{
	int i;
	isl_int *tr;

	tr = tab->mat->row[row] + 2 + tab->M;

	for (i = tab->n_param; i < tab->n_var - tab->n_div; ++i) {
		int s1, s2;
		isl_int *r;

		if (!tab->var[i].is_row) {
			if (tab->var[i].index == col1)
				return col2;
			if (tab->var[i].index == col2)
				return col1;
			continue;
		}

		if (tab->var[i].index == row)
			continue;

		r = tab->mat->row[tab->var[i].index] + 2 + tab->M;
		s1 = isl_int_sgn(r[col1]);
		s2 = isl_int_sgn(r[col2]);
		if (s1 == 0 && s2 == 0)
			continue;
		if (s1 < s2)
			return col1;
		if (s2 < s1)
			return col2;

		isl_int_mul(tmp, r[col2], tr[col1]);
		isl_int_submul(tmp, r[col1], tr[col2]);
		if (isl_int_is_pos(tmp))
			return col1;
		if (isl_int_is_neg(tmp))
			return col2;
	}
	return -1;
}

/* Given a row in the tableau, find and return the column that would
 * result in the lexicographically smallest, but positive, increment
 * in the sample point.
 * If there is no such column, then return tab->n_col.
 * If anything goes wrong, return -1.
 */
static int lexmin_pivot_col(struct isl_tab *tab, int row)
{
	int j;
	int col = tab->n_col;
	isl_int *tr;
	isl_int tmp;

	tr = tab->mat->row[row] + 2 + tab->M;

	isl_int_init(tmp);

	for (j = tab->n_dead; j < tab->n_col; ++j) {
		if (tab->col_var[j] >= 0 &&
		    (tab->col_var[j] < tab->n_param  ||
		    tab->col_var[j] >= tab->n_var - tab->n_div))
			continue;

		if (!isl_int_is_pos(tr[j]))
			continue;

		if (col == tab->n_col)
			col = j;
		else
			col = lexmin_col_pair(tab, row, col, j, tmp);
		isl_assert(tab->mat->ctx, col >= 0, goto error);
	}

	isl_int_clear(tmp);
	return col;
error:
	isl_int_clear(tmp);
	return -1;
}

/* Return the first known violated constraint, i.e., a non-negative
 * contraint that currently has an either obviously negative value
 * or a previously determined to be negative value.
 *
 * If any constraint has a negative coefficient for the big parameter,
 * if any, then we return one of these first.
 */
static int first_neg(struct isl_tab *tab)
{
	int row;

	if (tab->M)
		for (row = tab->n_redundant; row < tab->n_row; ++row) {
			if (!isl_tab_var_from_row(tab, row)->is_nonneg)
				continue;
			if (isl_int_is_neg(tab->mat->row[row][2]))
				return row;
		}
	for (row = tab->n_redundant; row < tab->n_row; ++row) {
		if (!isl_tab_var_from_row(tab, row)->is_nonneg)
			continue;
		if (tab->row_sign) {
			if (tab->row_sign[row] == 0 &&
			    is_obviously_neg(tab, row))
				tab->row_sign[row] = isl_tab_row_neg;
			if (tab->row_sign[row] != isl_tab_row_neg)
				continue;
		} else if (!is_obviously_neg(tab, row))
			continue;
		return row;
	}
	return -1;
}

/* Resolve all known or obviously violated constraints through pivoting.
 * In particular, as long as we can find any violated constraint, we
 * look for a pivoting column that would result in the lexicographicallly
 * smallest increment in the sample point.  If there is no such column
 * then the tableau is infeasible.
 */
static struct isl_tab *restore_lexmin(struct isl_tab *tab)
{
	int row, col;

	if (!tab)
		return NULL;
	if (tab->empty)
		return tab;
	while ((row = first_neg(tab)) != -1) {
		col = lexmin_pivot_col(tab, row);
		if (col >= tab->n_col)
			return isl_tab_mark_empty(tab);
		if (col < 0)
			goto error;
		isl_tab_pivot(tab, row, col);
	}
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Given a row that represents an equality, look for an appropriate
 * pivoting column.
 * In particular, if there are any non-zero coefficients among
 * the non-parameter variables, then we take the last of these
 * variables.  Eliminating this variable in terms of the other
 * variables and/or parameters does not influence the property
 * that all column in the initial tableau are lexicographically
 * positive.  The row corresponding to the eliminated variable
 * will only have non-zero entries below the diagonal of the
 * initial tableau.  That is, we transform
 *
 *		I				I
 *		  1		into		a
 *		    I				  I
 *
 * If there is no such non-parameter variable, then we are dealing with
 * pure parameter equality and we pick any parameter with coefficient 1 or -1
 * for elimination.  This will ensure that the eliminated parameter
 * always has an integer value whenever all the other parameters are integral.
 * If there is no such parameter then we return -1.
 */
static int last_var_col_or_int_par_col(struct isl_tab *tab, int row)
{
	unsigned off = 2 + tab->M;
	int i;

	for (i = tab->n_var - tab->n_div - 1; i >= 0 && i >= tab->n_param; --i) {
		int col;
		if (tab->var[i].is_row)
			continue;
		col = tab->var[i].index;
		if (col <= tab->n_dead)
			continue;
		if (!isl_int_is_zero(tab->mat->row[row][off + col]))
			return col;
	}
	for (i = tab->n_dead; i < tab->n_col; ++i) {
		if (isl_int_is_one(tab->mat->row[row][off + i]))
			return i;
		if (isl_int_is_negone(tab->mat->row[row][off + i]))
			return i;
	}
	return -1;
}

/* Add an equality that is known to be valid to the tableau.
 * We first check if we can eliminate a variable or a parameter.
 * If not, we add the equality as two inequalities.
 * In this case, the equality was a pure parameter equality and there
 * is no need to resolve any constraint violations.
 */
static struct isl_tab *add_lexmin_valid_eq(struct isl_tab *tab, isl_int *eq)
{
	int i;
	int r;

	if (!tab)
		return NULL;
	r = isl_tab_add_row(tab, eq);
	if (r < 0)
		goto error;

	r = tab->con[r].index;
	i = last_var_col_or_int_par_col(tab, r);
	if (i < 0) {
		tab->con[r].is_nonneg = 1;
		isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]);
		isl_seq_neg(eq, eq, 1 + tab->n_var);
		r = isl_tab_add_row(tab, eq);
		if (r < 0)
			goto error;
		tab->con[r].is_nonneg = 1;
		isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]);
	} else {
		isl_tab_pivot(tab, r, i);
		isl_tab_kill_col(tab, i);
		tab->n_eq++;

		tab = restore_lexmin(tab);
	}

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Check if the given row is a pure constant.
 */
static int is_constant(struct isl_tab *tab, int row)
{
	unsigned off = 2 + tab->M;

	return isl_seq_first_non_zero(tab->mat->row[row] + off + tab->n_dead,
					tab->n_col - tab->n_dead) == -1;
}

/* Add an equality that may or may not be valid to the tableau.
 * If the resulting row is a pure constant, then it must be zero.
 * Otherwise, the resulting tableau is empty.
 *
 * If the row is not a pure constant, then we add two inequalities,
 * each time checking that they can be satisfied.
 * In the end we try to use one of the two constraints to eliminate
 * a column.
 */
static struct isl_tab *add_lexmin_eq(struct isl_tab *tab, isl_int *eq)
{
	int r1, r2;
	int row;

	if (!tab)
		return NULL;
	if (tab->bset) {
		tab->bset = isl_basic_set_add_eq(tab->bset, eq);
		isl_tab_push(tab, isl_tab_undo_bset_eq);
		if (!tab->bset)
			goto error;
	}
	r1 = isl_tab_add_row(tab, eq);
	if (r1 < 0)
		goto error;
	tab->con[r1].is_nonneg = 1;
	isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r1]);

	row = tab->con[r1].index;
	if (is_constant(tab, row)) {
		if (!isl_int_is_zero(tab->mat->row[row][1]) ||
		    (tab->M && !isl_int_is_zero(tab->mat->row[row][2])))
			return isl_tab_mark_empty(tab);
		return tab;
	}

	tab = restore_lexmin(tab);
	if (!tab || tab->empty)
		return tab;

	isl_seq_neg(eq, eq, 1 + tab->n_var);

	r2 = isl_tab_add_row(tab, eq);
	if (r2 < 0)
		goto error;
	tab->con[r2].is_nonneg = 1;
	isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r2]);

	tab = restore_lexmin(tab);
	if (!tab || tab->empty)
		return tab;

	if (!tab->con[r1].is_row)
		isl_tab_kill_col(tab, tab->con[r1].index);
	else if (!tab->con[r2].is_row)
		isl_tab_kill_col(tab, tab->con[r2].index);
	else if (isl_int_is_zero(tab->mat->row[tab->con[r1].index][1])) {
		unsigned off = 2 + tab->M;
		int i;
		int row = tab->con[r1].index;
		i = isl_seq_first_non_zero(tab->mat->row[row]+off+tab->n_dead,
						tab->n_col - tab->n_dead);
		if (i != -1) {
			isl_tab_pivot(tab, row, tab->n_dead + i);
			isl_tab_kill_col(tab, tab->n_dead + i);
		}
	}

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Add an inequality to the tableau, resolving violations using
 * restore_lexmin.
 */
static struct isl_tab *add_lexmin_ineq(struct isl_tab *tab, isl_int *ineq)
{
	int r;

	if (!tab)
		return NULL;
	if (tab->bset) {
		tab->bset = isl_basic_set_add_ineq(tab->bset, ineq);
		isl_tab_push(tab, isl_tab_undo_bset_ineq);
		if (!tab->bset)
			goto error;
	}
	r = isl_tab_add_row(tab, ineq);
	if (r < 0)
		goto error;
	tab->con[r].is_nonneg = 1;
	isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]);
	if (isl_tab_row_is_redundant(tab, tab->con[r].index)) {
		isl_tab_mark_redundant(tab, tab->con[r].index);
		return tab;
	}

	tab = restore_lexmin(tab);
	if (tab && !tab->empty && tab->con[r].is_row &&
		 isl_tab_row_is_redundant(tab, tab->con[r].index))
		isl_tab_mark_redundant(tab, tab->con[r].index);
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Check if the coefficients of the parameters are all integral.
 */
static int integer_parameter(struct isl_tab *tab, int row)
{
	int i;
	int col;
	unsigned off = 2 + tab->M;

	for (i = 0; i < tab->n_param; ++i) {
		/* Eliminated parameter */
		if (tab->var[i].is_row)
			continue;
		col = tab->var[i].index;
		if (!isl_int_is_divisible_by(tab->mat->row[row][off + col],
						tab->mat->row[row][0]))
			return 0;
	}
	for (i = 0; i < tab->n_div; ++i) {
		if (tab->var[tab->n_var - tab->n_div + i].is_row)
			continue;
		col = tab->var[tab->n_var - tab->n_div + i].index;
		if (!isl_int_is_divisible_by(tab->mat->row[row][off + col],
						tab->mat->row[row][0]))
			return 0;
	}
	return 1;
}

/* Check if the coefficients of the non-parameter variables are all integral.
 */
static int integer_variable(struct isl_tab *tab, int row)
{
	int i;
	unsigned off = 2 + tab->M;

	for (i = 0; i < tab->n_col; ++i) {
		if (tab->col_var[i] >= 0 &&
		    (tab->col_var[i] < tab->n_param ||
		     tab->col_var[i] >= tab->n_var - tab->n_div))
			continue;
		if (!isl_int_is_divisible_by(tab->mat->row[row][off + i],
						tab->mat->row[row][0]))
			return 0;
	}
	return 1;
}

/* Check if the constant term is integral.
 */
static int integer_constant(struct isl_tab *tab, int row)
{
	return isl_int_is_divisible_by(tab->mat->row[row][1],
					tab->mat->row[row][0]);
}

#define I_CST	1 << 0
#define I_PAR	1 << 1
#define I_VAR	1 << 2

/* Check for first (non-parameter) variable that is non-integer and
 * therefore requires a cut.
 * For parametric tableaus, there are three parts in a row,
 * the constant, the coefficients of the parameters and the rest.
 * For each part, we check whether the coefficients in that part
 * are all integral and if so, set the corresponding flag in *f.
 * If the constant and the parameter part are integral, then the
 * current sample value is integral and no cut is required
 * (irrespective of whether the variable part is integral).
 */
static int first_non_integer(struct isl_tab *tab, int *f)
{
	int i;

	for (i = tab->n_param; i < tab->n_var - tab->n_div; ++i) {
		int flags = 0;
		int row;
		if (!tab->var[i].is_row)
			continue;
		row = tab->var[i].index;
		if (integer_constant(tab, row))
			ISL_FL_SET(flags, I_CST);
		if (integer_parameter(tab, row))
			ISL_FL_SET(flags, I_PAR);
		if (ISL_FL_ISSET(flags, I_CST) && ISL_FL_ISSET(flags, I_PAR))
			continue;
		if (integer_variable(tab, row))
			ISL_FL_SET(flags, I_VAR);
		*f = flags;
		return row;
	}
	return -1;
}

/* Add a (non-parametric) cut to cut away the non-integral sample
 * value of the given row.
 *
 * If the row is given by
 *
 *	m r = f + \sum_i a_i y_i
 *
 * then the cut is
 *
 *	c = - {-f/m} + \sum_i {a_i/m} y_i >= 0
 *
 * The big parameter, if any, is ignored, since it is assumed to be big
 * enough to be divisible by any integer.
 * If the tableau is actually a parametric tableau, then this function
 * is only called when all coefficients of the parameters are integral.
 * The cut therefore has zero coefficients for the parameters.
 *
 * The current value is known to be negative, so row_sign, if it
 * exists, is set accordingly.
 *
 * Return the row of the cut or -1.
 */
static int add_cut(struct isl_tab *tab, int row)
{
	int i;
	int r;
	isl_int *r_row;
	unsigned off = 2 + tab->M;

	if (isl_tab_extend_cons(tab, 1) < 0)
		return -1;
	r = isl_tab_allocate_con(tab);
	if (r < 0)
		return -1;

	r_row = tab->mat->row[tab->con[r].index];
	isl_int_set(r_row[0], tab->mat->row[row][0]);
	isl_int_neg(r_row[1], tab->mat->row[row][1]);
	isl_int_fdiv_r(r_row[1], r_row[1], tab->mat->row[row][0]);
	isl_int_neg(r_row[1], r_row[1]);
	if (tab->M)
		isl_int_set_si(r_row[2], 0);
	for (i = 0; i < tab->n_col; ++i)
		isl_int_fdiv_r(r_row[off + i],
			tab->mat->row[row][off + i], tab->mat->row[row][0]);

	tab->con[r].is_nonneg = 1;
	isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]);
	if (tab->row_sign)
		tab->row_sign[tab->con[r].index] = isl_tab_row_neg;

	return tab->con[r].index;
}

/* Given a non-parametric tableau, add cuts until an integer
 * sample point is obtained or until the tableau is determined
 * to be integer infeasible.
 * As long as there is any non-integer value in the sample point,
 * we add an appropriate cut, if possible and resolve the violated
 * cut constraint using restore_lexmin.
 * If one of the corresponding rows is equal to an integral
 * combination of variables/constraints plus a non-integral constant,
 * then there is no way to obtain an integer point an we return
 * a tableau that is marked empty.
 */
static struct isl_tab *cut_to_integer_lexmin(struct isl_tab *tab)
{
	int row;
	int flags;

	if (!tab)
		return NULL;
	if (tab->empty)
		return tab;

	while ((row = first_non_integer(tab, &flags)) != -1) {
		if (ISL_FL_ISSET(flags, I_VAR))
			return isl_tab_mark_empty(tab);
		row = add_cut(tab, row);
		if (row < 0)
			goto error;
		tab = restore_lexmin(tab);
		if (!tab || tab->empty)
			break;
	}
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

static struct isl_tab *drop_sample(struct isl_tab *tab, int s)
{
	if (s != tab->n_outside)
		isl_mat_swap_rows(tab->samples, tab->n_outside, s);
	tab->n_outside++;
	isl_tab_push(tab, isl_tab_undo_drop_sample);

	return tab;
}

/* Check whether all the currently active samples also satisfy the inequality
 * "ineq" (treated as an equality if eq is set).
 * Remove those samples that do not.
 */
static struct isl_tab *check_samples(struct isl_tab *tab, isl_int *ineq, int eq)
{
	int i;
	isl_int v;

	if (!tab)
		return NULL;

	isl_assert(tab->mat->ctx, tab->bset, goto error);
	isl_assert(tab->mat->ctx, tab->samples, goto error);
	isl_assert(tab->mat->ctx, tab->samples->n_col == 1 + tab->n_var, goto error);

	isl_int_init(v);
	for (i = tab->n_outside; i < tab->n_sample; ++i) {
		int sgn;
		isl_seq_inner_product(ineq, tab->samples->row[i],
					1 + tab->n_var, &v);
		sgn = isl_int_sgn(v);
		if (eq ? (sgn == 0) : (sgn >= 0))
			continue;
		tab = drop_sample(tab, i);
		if (!tab)
			break;
	}
	isl_int_clear(v);

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Check whether the sample value of the tableau is finite,
 * i.e., either the tableau does not use a big parameter, or
 * all values of the variables are equal to the big parameter plus
 * some constant.  This constant is the actual sample value.
 */
static int sample_is_finite(struct isl_tab *tab)
{
	int i;

	if (!tab->M)
		return 1;

	for (i = 0; i < tab->n_var; ++i) {
		int row;
		if (!tab->var[i].is_row)
			return 0;
		row = tab->var[i].index;
		if (isl_int_ne(tab->mat->row[row][0], tab->mat->row[row][2]))
			return 0;
	}
	return 1;
}

/* Check if the context tableau of sol has any integer points.
 * Returns -1 if an error occurred.
 * If an integer point can be found and if moreover it is finite,
 * then it is added to the list of sample values.
 *
 * This function is only called when none of the currently active sample
 * values satisfies the most recently added constraint.
 */
static int context_is_feasible(struct isl_sol *sol)
{
	struct isl_tab_undo *snap;
	struct isl_tab *tab;
	int feasible;

	if (!sol || !sol->context_tab)
		return -1;

	snap = isl_tab_snap(sol->context_tab);
	isl_tab_push_basis(sol->context_tab);

	sol->context_tab = cut_to_integer_lexmin(sol->context_tab);
	if (!sol->context_tab)
		goto error;

	tab = sol->context_tab;
	if (!tab->empty && sample_is_finite(tab)) {
		struct isl_vec *sample;

		tab->samples = isl_mat_extend(tab->samples,
					tab->n_sample + 1, tab->samples->n_col);
		if (!tab->samples)
			goto error;

		sample = isl_tab_get_sample_value(tab);
		if (!sample)
			goto error;
		isl_seq_cpy(tab->samples->row[tab->n_sample],
				sample->el, sample->size);
		isl_vec_free(sample);
		tab->n_sample++;
	}

	feasible = !sol->context_tab->empty;
	if (isl_tab_rollback(sol->context_tab, snap) < 0)
		goto error;

	return feasible;
error:
	isl_tab_free(sol->context_tab);
	sol->context_tab = NULL;
	return -1;
}

/* First check if any of the currently active sample values satisfies
 * the inequality "ineq" (an equality if eq is set).
 * If not, continue with check_integer_feasible.
 */
static int context_valid_sample_or_feasible(struct isl_sol *sol,
	isl_int *ineq, int eq)
{
	int i;
	isl_int v;
	struct isl_tab *tab;

	if (!sol || !sol->context_tab)
		return -1;

	tab = sol->context_tab;
	isl_assert(tab->mat->ctx, tab->bset, return -1);
	isl_assert(tab->mat->ctx, tab->samples, return -1);
	isl_assert(tab->mat->ctx, tab->samples->n_col == 1 + tab->n_var, return -1);

	isl_int_init(v);
	for (i = tab->n_outside; i < tab->n_sample; ++i) {
		int sgn;
		isl_seq_inner_product(ineq, tab->samples->row[i],
					1 + tab->n_var, &v);
		sgn = isl_int_sgn(v);
		if (eq ? (sgn == 0) : (sgn >= 0))
			break;
	}
	isl_int_clear(v);

	if (i < tab->n_sample)
		return 1;

	return context_is_feasible(sol);
}

/* For a div d = floor(f/m), add the constraints
 *
 *		f - m d >= 0
 *		-(f-(m-1)) + m d >= 0
 *
 * Note that the second constraint is the negation of
 *
 *		f - m d >= m
 */
static struct isl_tab *add_div_constraints(struct isl_tab *tab, unsigned div)
{
	unsigned total;
	unsigned div_pos;
	struct isl_vec *ineq;

	if (!tab)
		return NULL;

	total = isl_basic_set_total_dim(tab->bset);
	div_pos = 1 + total - tab->bset->n_div + div;

	ineq = ineq_for_div(tab->bset, div);
	if (!ineq)
		goto error;

	tab = add_lexmin_ineq(tab, ineq->el);

	isl_seq_neg(ineq->el, tab->bset->div[div] + 1, 1 + total);
	isl_int_set(ineq->el[div_pos], tab->bset->div[div][0]);
	isl_int_add(ineq->el[0], ineq->el[0], ineq->el[div_pos]);
	isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);
	tab = add_lexmin_ineq(tab, ineq->el);

	isl_vec_free(ineq);

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Add a div specified by "div" to both the main tableau and
 * the context tableau.  In case of the main tableau, we only
 * need to add an extra div.  In the context tableau, we also
 * need to express the meaning of the div.
 * Return the index of the div or -1 if anything went wrong.
 */
static int add_div(struct isl_tab *tab, struct isl_tab **context_tab,
	struct isl_vec *div)
{
	int i;
	int r;
	int k;
	struct isl_mat *samples;

	if (isl_tab_extend_vars(*context_tab, 1) < 0)
		goto error;
	r = isl_tab_allocate_var(*context_tab);
	if (r < 0)
		goto error;
	(*context_tab)->var[r].is_nonneg = 1;
	(*context_tab)->var[r].frozen = 1;

	samples = isl_mat_extend((*context_tab)->samples,
			(*context_tab)->n_sample, 1 + (*context_tab)->n_var);
	(*context_tab)->samples = samples;
	if (!samples)
		goto error;
	for (i = (*context_tab)->n_outside; i < samples->n_row; ++i) {
		isl_seq_inner_product(div->el + 1, samples->row[i],
			div->size - 1, &samples->row[i][samples->n_col - 1]);
		isl_int_fdiv_q(samples->row[i][samples->n_col - 1],
			       samples->row[i][samples->n_col - 1], div->el[0]);
	}

	(*context_tab)->bset = isl_basic_set_extend_dim((*context_tab)->bset,
		isl_basic_set_get_dim((*context_tab)->bset), 1, 0, 2);
	k = isl_basic_set_alloc_div((*context_tab)->bset);
	if (k < 0)
		goto error;
	isl_seq_cpy((*context_tab)->bset->div[k], div->el, div->size);
	isl_tab_push((*context_tab), isl_tab_undo_bset_div);
	*context_tab = add_div_constraints(*context_tab, k);
	if (!*context_tab)
		goto error;

	if (isl_tab_extend_vars(tab, 1) < 0)
		goto error;
	r = isl_tab_allocate_var(tab);
	if (r < 0)
		goto error;
	if (!(*context_tab)->M)
		tab->var[r].is_nonneg = 1;
	tab->var[r].frozen = 1;
	tab->n_div++;

	return tab->n_div - 1;
error:
	isl_tab_free(*context_tab);
	*context_tab = NULL;
	return -1;
}

static int find_div(struct isl_tab *tab, isl_int *div, isl_int denom)
{
	int i;
	unsigned total = isl_basic_set_total_dim(tab->bset);

	for (i = 0; i < tab->bset->n_div; ++i) {
		if (isl_int_ne(tab->bset->div[i][0], denom))
			continue;
		if (!isl_seq_eq(tab->bset->div[i] + 1, div, total))
			continue;
		return i;
	}
	return -1;
}

/* Return the index of a div that corresponds to "div".
 * We first check if we already have such a div and if not, we create one.
 */
static int get_div(struct isl_tab *tab, struct isl_tab **context_tab,
	struct isl_vec *div)
{
	int d;

	d = find_div(*context_tab, div->el + 1, div->el[0]);
	if (d != -1)
		return d;

	return add_div(tab, context_tab, div);
}

/* Add a parametric cut to cut away the non-integral sample value
 * of the give row.
 * Let a_i be the coefficients of the constant term and the parameters
 * and let b_i be the coefficients of the variables or constraints
 * in basis of the tableau.
 * Let q be the div q = floor(\sum_i {-a_i} y_i).
 *
 * The cut is expressed as
 *
 *	c = \sum_i -{-a_i} y_i + \sum_i {b_i} x_i + q >= 0
 *
 * If q did not already exist in the context tableau, then it is added first.
 * If q is in a column of the main tableau then the "+ q" can be accomplished
 * by setting the corresponding entry to the denominator of the constraint.
 * If q happens to be in a row of the main tableau, then the corresponding
 * row needs to be added instead (taking care of the denominators).
 * Note that this is very unlikely, but perhaps not entirely impossible.
 *
 * The current value of the cut is known to be negative (or at least
 * non-positive), so row_sign is set accordingly.
 *
 * Return the row of the cut or -1.
 */
static int add_parametric_cut(struct isl_tab *tab, int row,
	struct isl_tab **context_tab)
{
	struct isl_vec *div;
	int d;
	int i;
	int r;
	isl_int *r_row;
	int col;
	unsigned off = 2 + tab->M;

	if (!*context_tab)
		goto error;

	if (isl_tab_extend_cons(*context_tab, 3) < 0)
		goto error;

	div = get_row_parameter_div(tab, row);
	if (!div)
		return -1;

	d = get_div(tab, context_tab, div);
	if (d < 0)
		goto error;

	if (isl_tab_extend_cons(tab, 1) < 0)
		return -1;
	r = isl_tab_allocate_con(tab);
	if (r < 0)
		return -1;

	r_row = tab->mat->row[tab->con[r].index];
	isl_int_set(r_row[0], tab->mat->row[row][0]);
	isl_int_neg(r_row[1], tab->mat->row[row][1]);
	isl_int_fdiv_r(r_row[1], r_row[1], tab->mat->row[row][0]);
	isl_int_neg(r_row[1], r_row[1]);
	if (tab->M)
		isl_int_set_si(r_row[2], 0);
	for (i = 0; i < tab->n_param; ++i) {
		if (tab->var[i].is_row)
			continue;
		col = tab->var[i].index;
		isl_int_neg(r_row[off + col], tab->mat->row[row][off + col]);
		isl_int_fdiv_r(r_row[off + col], r_row[off + col],
				tab->mat->row[row][0]);
		isl_int_neg(r_row[off + col], r_row[off + col]);
	}
	for (i = 0; i < tab->n_div; ++i) {
		if (tab->var[tab->n_var - tab->n_div + i].is_row)
			continue;
		col = tab->var[tab->n_var - tab->n_div + i].index;
		isl_int_neg(r_row[off + col], tab->mat->row[row][off + col]);
		isl_int_fdiv_r(r_row[off + col], r_row[off + col],
				tab->mat->row[row][0]);
		isl_int_neg(r_row[off + col], r_row[off + col]);
	}
	for (i = 0; i < tab->n_col; ++i) {
		if (tab->col_var[i] >= 0 &&
		    (tab->col_var[i] < tab->n_param ||
		     tab->col_var[i] >= tab->n_var - tab->n_div))
			continue;
		isl_int_fdiv_r(r_row[off + i],
			tab->mat->row[row][off + i], tab->mat->row[row][0]);
	}
	if (tab->var[tab->n_var - tab->n_div + d].is_row) {
		isl_int gcd;
		int d_row = tab->var[tab->n_var - tab->n_div + d].index;
		isl_int_init(gcd);
		isl_int_gcd(gcd, tab->mat->row[d_row][0], r_row[0]);
		isl_int_divexact(r_row[0], r_row[0], gcd);
		isl_int_divexact(gcd, tab->mat->row[d_row][0], gcd);
		isl_seq_combine(r_row + 1, gcd, r_row + 1,
				r_row[0], tab->mat->row[d_row] + 1,
				off - 1 + tab->n_col);
		isl_int_mul(r_row[0], r_row[0], tab->mat->row[d_row][0]);
		isl_int_clear(gcd);
	} else {
		col = tab->var[tab->n_var - tab->n_div + d].index;
		isl_int_set(r_row[off + col], tab->mat->row[row][0]);
	}

	tab->con[r].is_nonneg = 1;
	isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]);
	if (tab->row_sign)
		tab->row_sign[tab->con[r].index] = isl_tab_row_neg;

	isl_vec_free(div);

	return tab->con[r].index;
error:
	isl_tab_free(*context_tab);
	*context_tab = NULL;
	return -1;
}

/* Construct a tableau for bmap that can be used for computing
 * the lexicographic minimum (or maximum) of bmap.
 * If not NULL, then dom is the domain where the minimum
 * should be computed.  In this case, we set up a parametric
 * tableau with row signs (initialized to "unknown").
 * If M is set, then the tableau will use a big parameter.
 * If max is set, then a maximum should be computed instead of a minimum.
 * This means that for each variable x, the tableau will contain the variable
 * x' = M - x, rather than x' = M + x.  This in turn means that the coefficient
 * of the variables in all constraints are negated prior to adding them
 * to the tableau.
 */
static struct isl_tab *tab_for_lexmin(struct isl_basic_map *bmap,
	struct isl_basic_set *dom, unsigned M, int max)
{
	int i;
	struct isl_tab *tab;

	tab = isl_tab_alloc(bmap->ctx, 2 * bmap->n_eq + bmap->n_ineq + 1,
			    isl_basic_map_total_dim(bmap), M);
	if (!tab)
		return NULL;

	tab->rational = ISL_F_ISSET(bmap, ISL_BASIC_MAP_RATIONAL);
	if (dom) {
		tab->n_param = isl_basic_set_total_dim(dom) - dom->n_div;
		tab->n_div = dom->n_div;
		tab->row_sign = isl_calloc_array(bmap->ctx,
					enum isl_tab_row_sign, tab->mat->n_row);
		if (!tab->row_sign)
			goto error;
	}
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_EMPTY))
		return isl_tab_mark_empty(tab);

	for (i = tab->n_param; i < tab->n_var - tab->n_div; ++i) {
		tab->var[i].is_nonneg = 1;
		tab->var[i].frozen = 1;
	}
	for (i = 0; i < bmap->n_eq; ++i) {
		if (max)
			isl_seq_neg(bmap->eq[i] + 1 + tab->n_param,
				    bmap->eq[i] + 1 + tab->n_param,
				    tab->n_var - tab->n_param - tab->n_div);
		tab = add_lexmin_valid_eq(tab, bmap->eq[i]);
		if (max)
			isl_seq_neg(bmap->eq[i] + 1 + tab->n_param,
				    bmap->eq[i] + 1 + tab->n_param,
				    tab->n_var - tab->n_param - tab->n_div);
		if (!tab || tab->empty)
			return tab;
	}
	for (i = 0; i < bmap->n_ineq; ++i) {
		if (max)
			isl_seq_neg(bmap->ineq[i] + 1 + tab->n_param,
				    bmap->ineq[i] + 1 + tab->n_param,
				    tab->n_var - tab->n_param - tab->n_div);
		tab = add_lexmin_ineq(tab, bmap->ineq[i]);
		if (max)
			isl_seq_neg(bmap->ineq[i] + 1 + tab->n_param,
				    bmap->ineq[i] + 1 + tab->n_param,
				    tab->n_var - tab->n_param - tab->n_div);
		if (!tab || tab->empty)
			return tab;
	}
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

static struct isl_tab *context_tab_for_lexmin(struct isl_basic_set *bset)
{
	struct isl_tab *tab;

	bset = isl_basic_set_cow(bset);
	if (!bset)
		return NULL;
	tab = tab_for_lexmin((struct isl_basic_map *)bset, NULL, 1, 0);
	if (!tab)
		goto error;
	tab->bset = bset;
	tab->n_sample = 0;
	tab->n_outside = 0;
	tab->samples = isl_mat_alloc(bset->ctx, 1, 1 + tab->n_var);
	if (!tab->samples)
		goto error;
	return tab;
error:
	isl_basic_set_free(bset);
	return NULL;
}

/* Construct an isl_sol_map structure for accumulating the solution.
 * If track_empty is set, then we also keep track of the parts
 * of the context where there is no solution.
 * If max is set, then we are solving a maximization, rather than
 * a minimization problem, which means that the variables in the
 * tableau have value "M - x" rather than "M + x".
 */
static struct isl_sol_map *sol_map_init(struct isl_basic_map *bmap,
	struct isl_basic_set *dom, int track_empty, int max)
{
	struct isl_sol_map *sol_map;
	struct isl_tab *context_tab;
	int f;

	sol_map = isl_calloc_type(bset->ctx, struct isl_sol_map);
	if (!sol_map)
		goto error;

	sol_map->max = max;
	sol_map->sol.add = &sol_map_add_wrap;
	sol_map->sol.free = &sol_map_free_wrap;
	sol_map->map = isl_map_alloc_dim(isl_basic_map_get_dim(bmap), 1,
					    ISL_MAP_DISJOINT);
	if (!sol_map->map)
		goto error;

	context_tab = context_tab_for_lexmin(isl_basic_set_copy(dom));
	context_tab = restore_lexmin(context_tab);
	sol_map->sol.context_tab = context_tab;
	f = context_is_feasible(&sol_map->sol);
	if (f < 0)
		goto error;

	if (track_empty) {
		sol_map->empty = isl_set_alloc_dim(isl_basic_set_get_dim(dom),
							1, ISL_SET_DISJOINT);
		if (!sol_map->empty)
			goto error;
	}

	isl_basic_set_free(dom);
	return sol_map;
error:
	isl_basic_set_free(dom);
	sol_map_free(sol_map);
	return NULL;
}

/* For each variable in the context tableau, check if the variable can
 * only attain non-negative values.  If so, mark the parameter as non-negative
 * in the main tableau.  This allows for a more direct identification of some
 * cases of violated constraints.
 */
static struct isl_tab *tab_detect_nonnegative_parameters(struct isl_tab *tab,
	struct isl_tab *context_tab)
{
	int i;
	struct isl_tab_undo *snap, *snap2;
	struct isl_vec *ineq = NULL;
	struct isl_tab_var *var;
	int n;

	if (context_tab->n_var == 0)
		return tab;

	ineq = isl_vec_alloc(tab->mat->ctx, 1 + context_tab->n_var);
	if (!ineq)
		goto error;

	if (isl_tab_extend_cons(context_tab, 1) < 0)
		goto error;

	snap = isl_tab_snap(context_tab);
	isl_tab_push_basis(context_tab);

	snap2 = isl_tab_snap(context_tab);

	n = 0;
	isl_seq_clr(ineq->el, ineq->size);
	for (i = 0; i < context_tab->n_var; ++i) {
		isl_int_set_si(ineq->el[1 + i], 1);
		context_tab = isl_tab_add_ineq(context_tab, ineq->el);
		var = &context_tab->con[context_tab->n_con - 1];
		if (!context_tab->empty &&
		    !isl_tab_min_at_most_neg_one(context_tab, var)) {
			int j = i;
			if (i >= tab->n_param)
				j = i - tab->n_param + tab->n_var - tab->n_div;
			tab->var[j].is_nonneg = 1;
			n++;
		}
		isl_int_set_si(ineq->el[1 + i], 0);
		if (isl_tab_rollback(context_tab, snap2) < 0)
			goto error;
	}

	if (isl_tab_rollback(context_tab, snap) < 0)
		goto error;

	if (n == context_tab->n_var) {
		context_tab->mat = isl_mat_drop_cols(context_tab->mat, 2, 1);
		context_tab->M = 0;
	}

	isl_vec_free(ineq);
	return tab;
error:
	isl_vec_free(ineq);
	isl_tab_free(tab);
	return NULL;
}

/* Check whether all coefficients of (non-parameter) variables
 * are non-positive, meaning that no pivots can be performed on the row.
 */
static int is_critical(struct isl_tab *tab, int row)
{
	int j;
	unsigned off = 2 + tab->M;

	for (j = tab->n_dead; j < tab->n_col; ++j) {
		if (tab->col_var[j] >= 0 &&
		    (tab->col_var[j] < tab->n_param  ||
		    tab->col_var[j] >= tab->n_var - tab->n_div))
			continue;

		if (isl_int_is_pos(tab->mat->row[row][off + j]))
			return 0;
	}

	return 1;
}

/* Check whether the inequality represented by vec is strict over the integers,
 * i.e., there are no integer values satisfying the constraint with
 * equality.  This happens if the gcd of the coefficients is not a divisor
 * of the constant term.  If so, scale the constraint down by the gcd
 * of the coefficients.
 */
static int is_strict(struct isl_vec *vec)
{
	isl_int gcd;
	int strict = 0;

	isl_int_init(gcd);
	isl_seq_gcd(vec->el + 1, vec->size - 1, &gcd);
	if (!isl_int_is_one(gcd)) {
		strict = !isl_int_is_divisible_by(vec->el[0], gcd);
		isl_int_fdiv_q(vec->el[0], vec->el[0], gcd);
		isl_seq_scale_down(vec->el + 1, vec->el + 1, gcd, vec->size-1);
	}
	isl_int_clear(gcd);

	return strict;
}

/* Determine the sign of the given row of the main tableau.
 * The result is one of
 *	isl_tab_row_pos: always non-negative; no pivot needed
 *	isl_tab_row_neg: always non-positive; pivot
 *	isl_tab_row_any: can be both positive and negative; split
 *
 * We first handle some simple cases
 *	- the row sign may be known already
 *	- the row may be obviously non-negative
 *	- the parametric constant may be equal to that of another row
 *	  for which we know the sign.  This sign will be either "pos" or
 *	  "any".  If it had been "neg" then we would have pivoted before.
 *
 * If none of these cases hold, we check the value of the row for each
 * of the currently active samples.  Based on the signs of these values
 * we make an initial determination of the sign of the row.
 *
 *	all zero			->	unk(nown)
 *	all non-negative		->	pos
 *	all non-positive		->	neg
 *	both negative and positive	->	all
 *
 * If we end up with "all", we are done.
 * Otherwise, we perform a check for positive and/or negative
 * values as follows.
 *
 *	samples	       neg	       unk	       pos
 *	<0 ?			    Y        N	    Y        N
 *					    pos    any      pos
 *	>0 ?	     Y      N	 Y     N
 *		    any    neg  any   neg
 *
 * There is no special sign for "zero", because we can usually treat zero
 * as either non-negative or non-positive, whatever works out best.
 * However, if the row is "critical", meaning that pivoting is impossible
 * then we don't want to limp zero with the non-positive case, because
 * then we we would lose the solution for those values of the parameters
 * where the value of the row is zero.  Instead, we treat 0 as non-negative
 * ensuring a split if the row can attain both zero and negative values.
 * The same happens when the original constraint was one that could not
 * be satisfied with equality by any integer values of the parameters.
 * In this case, we normalize the constraint, but then a value of zero
 * for the normalized constraint is actually a positive value for the
 * original constraint, so again we need to treat zero as non-negative.
 * In both these cases, we have the following decision tree instead:
 *
 *	all non-negative		->	pos
 *	all negative			->	neg
 *	both negative and non-negative	->	all
 *
 *	samples	       neg	          	       pos
 *	<0 ?			             	    Y        N
 *					           any      pos
 *	>=0 ?	     Y      N
 *		    any    neg
 */
static int row_sign(struct isl_tab *tab, struct isl_sol *sol, int row)
{
	int i;
	struct isl_tab_undo *snap = NULL;
	struct isl_vec *ineq = NULL;
	int res = isl_tab_row_unknown;
	int critical;
	int strict;
	int sgn;
	int row2;
	isl_int tmp;
	struct isl_tab *context_tab = sol->context_tab;

	if (tab->row_sign[row] != isl_tab_row_unknown)
		return tab->row_sign[row];
	if (is_obviously_nonneg(tab, row))
		return isl_tab_row_pos;
	for (row2 = tab->n_redundant; row2 < tab->n_row; ++row2) {
		if (tab->row_sign[row2] == isl_tab_row_unknown)
			continue;
		if (identical_parameter_line(tab, row, row2))
			return tab->row_sign[row2];
	}

	critical = is_critical(tab, row);

	isl_assert(tab->mat->ctx, context_tab->samples, goto error);
	isl_assert(tab->mat->ctx, context_tab->samples->n_col == 1 + context_tab->n_var, goto error);

	ineq = get_row_parameter_ineq(tab, row);
	if (!ineq)
		goto error;

	strict = is_strict(ineq);

	isl_int_init(tmp);
	for (i = context_tab->n_outside; i < context_tab->n_sample; ++i) {
		isl_seq_inner_product(context_tab->samples->row[i], ineq->el,
					ineq->size, &tmp);
		sgn = isl_int_sgn(tmp);
		if (sgn > 0 || (sgn == 0 && (critical || strict))) {
			if (res == isl_tab_row_unknown)
				res = isl_tab_row_pos;
			if (res == isl_tab_row_neg)
				res = isl_tab_row_any;
		}
		if (sgn < 0) {
			if (res == isl_tab_row_unknown)
				res = isl_tab_row_neg;
			if (res == isl_tab_row_pos)
				res = isl_tab_row_any;
		}
		if (res == isl_tab_row_any)
			break;
	}
	isl_int_clear(tmp);

	if (res != isl_tab_row_any) {
		if (isl_tab_extend_cons(context_tab, 1) < 0)
			goto error;

		snap = isl_tab_snap(context_tab);
		isl_tab_push_basis(context_tab);
	}

	if (res == isl_tab_row_unknown || res == isl_tab_row_pos) {
		/* test for negative values */
		int feasible;
		isl_seq_neg(ineq->el, ineq->el, ineq->size);
		isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);

		isl_tab_push_basis(context_tab);
		sol->context_tab = add_lexmin_ineq(sol->context_tab, ineq->el);
		feasible = context_is_feasible(sol);
		if (feasible < 0)
			goto error;
		context_tab = sol->context_tab;
		if (!feasible)
			res = isl_tab_row_pos;
		else
			res = (res == isl_tab_row_unknown) ? isl_tab_row_neg
							   : isl_tab_row_any;
		if (isl_tab_rollback(context_tab, snap) < 0)
			goto error;

		if (res == isl_tab_row_neg) {
			isl_seq_neg(ineq->el, ineq->el, ineq->size);
			isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);
		}
	}

	if (res == isl_tab_row_neg) {
		/* test for positive values */
		int feasible;
		if (!critical && !strict)
			isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);

		isl_tab_push_basis(context_tab);
		sol->context_tab = add_lexmin_ineq(sol->context_tab, ineq->el);
		feasible = context_is_feasible(sol);
		if (feasible < 0)
			goto error;
		context_tab = sol->context_tab;
		if (feasible)
			res = isl_tab_row_any;
		if (isl_tab_rollback(context_tab, snap) < 0)
			goto error;
	}

	isl_vec_free(ineq);
	return res;
error:
	isl_vec_free(ineq);
	return 0;
}

static struct isl_sol *find_solutions(struct isl_sol *sol, struct isl_tab *tab);

/* Find solutions for values of the parameters that satisfy the given
 * inequality.
 *
 * We currently take a snapshot of the context tableau that is reset
 * when we return from this function, while we make a copy of the main
 * tableau, leaving the original main tableau untouched.
 * These are fairly arbitrary choices.  Making a copy also of the context
 * tableau would obviate the need to undo any changes made to it later,
 * while taking a snapshot of the main tableau could reduce memory usage.
 * If we were to switch to taking a snapshot of the main tableau,
 * we would have to keep in mind that we need to save the row signs
 * and that we need to do this before saving the current basis
 * such that the basis has been restore before we restore the row signs.
 */
static struct isl_sol *find_in_pos(struct isl_sol *sol,
	struct isl_tab *tab, isl_int *ineq)
{
	struct isl_tab_undo *snap;

	snap = isl_tab_snap(sol->context_tab);
	isl_tab_push_basis(sol->context_tab);
	if (isl_tab_extend_cons(sol->context_tab, 1) < 0)
		goto error;

	tab = isl_tab_dup(tab);
	if (!tab)
		goto error;

	sol->context_tab = add_lexmin_ineq(sol->context_tab, ineq);
	sol->context_tab = check_samples(sol->context_tab, ineq, 0);

	sol = find_solutions(sol, tab);

	isl_tab_rollback(sol->context_tab, snap);
	return sol;
error:
	isl_tab_rollback(sol->context_tab, snap);
	sol_free(sol);
	return NULL;
}

/* Record the absence of solutions for those values of the parameters
 * that do not satisfy the given inequality with equality.
 */
static struct isl_sol *no_sol_in_strict(struct isl_sol *sol,
	struct isl_tab *tab, struct isl_vec *ineq)
{
	int empty;
	int f;
	struct isl_tab_undo *snap;
	snap = isl_tab_snap(sol->context_tab);
	isl_tab_push_basis(sol->context_tab);
	if (isl_tab_extend_cons(sol->context_tab, 1) < 0)
		goto error;

	isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);

	sol->context_tab = add_lexmin_ineq(sol->context_tab, ineq->el);
	f = context_valid_sample_or_feasible(sol, ineq->el, 0);
	if (f < 0)
		goto error;

	empty = tab->empty;
	tab->empty = 1;
	sol = sol->add(sol, tab);
	tab->empty = empty;

	isl_int_add_ui(ineq->el[0], ineq->el[0], 1);

	if (isl_tab_rollback(sol->context_tab, snap) < 0)
		goto error;
	return sol;
error:
	sol_free(sol);
	return NULL;
}

/* Given a main tableau where more than one row requires a split,
 * determine and return the "best" row to split on.
 *
 * Given two rows in the main tableau, if the inequality corresponding
 * to the first row is redundant with respect to that of the second row
 * in the current tableau, then it is better to split on the second row,
 * since in the positive part, both row will be positive.
 * (In the negative part a pivot will have to be performed and just about
 * anything can happen to the sign of the other row.)
 *
 * As a simple heuristic, we therefore select the row that makes the most
 * of the other rows redundant.
 *
 * Perhaps it would also be useful to look at the number of constraints
 * that conflict with any given constraint.
 */
static int best_split(struct isl_tab *tab, struct isl_tab *context_tab)
{
	struct isl_tab_undo *snap, *snap2;
	int split;
	int row;
	int best = -1;
	int best_r;

	if (isl_tab_extend_cons(context_tab, 2) < 0)
		return -1;

	snap = isl_tab_snap(context_tab);
	isl_tab_push_basis(context_tab);
	snap2 = isl_tab_snap(context_tab);

	for (split = tab->n_redundant; split < tab->n_row; ++split) {
		struct isl_tab_undo *snap3;
		struct isl_vec *ineq = NULL;
		int r = 0;

		if (!isl_tab_var_from_row(tab, split)->is_nonneg)
			continue;
		if (tab->row_sign[split] != isl_tab_row_any)
			continue;

		ineq = get_row_parameter_ineq(tab, split);
		if (!ineq)
			return -1;
		context_tab = isl_tab_add_ineq(context_tab, ineq->el);
		isl_vec_free(ineq);

		snap3 = isl_tab_snap(context_tab);

		for (row = tab->n_redundant; row < tab->n_row; ++row) {
			struct isl_tab_var *var;

			if (row == split)
				continue;
			if (!isl_tab_var_from_row(tab, row)->is_nonneg)
				continue;
			if (tab->row_sign[row] != isl_tab_row_any)
				continue;

			ineq = get_row_parameter_ineq(tab, row);
			if (!ineq)
				return -1;
			context_tab = isl_tab_add_ineq(context_tab, ineq->el);
			isl_vec_free(ineq);
			var = &context_tab->con[context_tab->n_con - 1];
			if (!context_tab->empty &&
			    !isl_tab_min_at_most_neg_one(context_tab, var))
				r++;
			if (isl_tab_rollback(context_tab, snap3) < 0)
				return -1;
		}
		if (best == -1 || r > best_r) {
			best = split;
			best_r = r;
		}
		if (isl_tab_rollback(context_tab, snap2) < 0)
			return -1;
	}

	if (isl_tab_rollback(context_tab, snap) < 0)
		return -1;

	return best;
}

/* Compute the lexicographic minimum of the set represented by the main
 * tableau "tab" within the context "sol->context_tab".
 * On entry the sample value of the main tableau is lexicographically
 * less than or equal to this lexicographic minimum.
 * Pivots are performed until a feasible point is found, which is then
 * necessarily equal to the minimum, or until the tableau is found to
 * be infeasible.  Some pivots may need to be performed for only some
 * feasible values of the context tableau.  If so, the context tableau
 * is split into a part where the pivot is needed and a part where it is not.
 *
 * Whenever we enter the main loop, the main tableau is such that no
 * "obvious" pivots need to be performed on it, where "obvious" means
 * that the given row can be seen to be negative without looking at
 * the context tableau.  In particular, for non-parametric problems,
 * no pivots need to be performed on the main tableau.
 * The caller of find_solutions is responsible for making this property
 * hold prior to the first iteration of the loop, while restore_lexmin
 * is called before every other iteration.
 *
 * Inside the main loop, we first examine the signs of the rows of
 * the main tableau within the context of the context tableau.
 * If we find a row that is always non-positive for all values of
 * the parameters satisfying the context tableau and negative for at
 * least one value of the parameters, we perform the appropriate pivot
 * and start over.  An exception is the case where no pivot can be
 * performed on the row.  In this case, we require that the sign of
 * the row is negative for all values of the parameters (rather than just
 * non-positive).  This special case is handled inside row_sign, which
 * will say that the row can have any sign if it determines that it can
 * attain both negative and zero values.
 *
 * If we can't find a row that always requires a pivot, but we can find
 * one or more rows that require a pivot for some values of the parameters
 * (i.e., the row can attain both positive and negative signs), then we split
 * the context tableau into two parts, one where we force the sign to be
 * non-negative and one where we force is to be negative.
 * The non-negative part is handled by a recursive call (through find_in_pos).
 * Upon returning from this call, we continue with the negative part and
 * perform the required pivot.
 *
 * If no such rows can be found, all rows are non-negative and we have
 * found a (rational) feasible point.  If we only wanted a rational point
 * then we are done.
 * Otherwise, we check if all values of the sample point of the tableau
 * are integral for the variables.  If so, we have found the minimal
 * integral point and we are done.
 * If the sample point is not integral, then we need to make a distinction
 * based on whether the constant term is non-integral or the coefficients
 * of the parameters.  Furthermore, in order to decide how to handle
 * the non-integrality, we also need to know whether the coefficients
 * of the other columns in the tableau are integral.  This leads
 * to the following table.  The first two rows do not correspond
 * to a non-integral sample point and are only mentioned for completeness.
 *
 *	constant	parameters	other
 *
 *	int		int		int	|
 *	int		int		rat	| -> no problem
 *
 *	rat		int		int	  -> fail
 *
 *	rat		int		rat	  -> cut
 *
 *	int		rat		rat	|
 *	rat		rat		rat	| -> parametric cut
 *
 *	int		rat		int	|
 *	rat		rat		int	| -> split context
 *
 * If the parametric constant is completely integral, then there is nothing
 * to be done.  If the constant term is non-integral, but all the other
 * coefficient are integral, then there is nothing that can be done
 * and the tableau has no integral solution.
 * If, on the other hand, one or more of the other columns have rational
 * coeffcients, but the parameter coefficients are all integral, then
 * we can perform a regular (non-parametric) cut.
 * Finally, if there is any parameter coefficient that is non-integral,
 * then we need to involve the context tableau.  There are two cases here.
 * If at least one other column has a rational coefficient, then we
 * can perform a parametric cut in the main tableau by adding a new
 * integer division in the context tableau.
 * If all other columns have integral coefficients, then we need to
 * enforce that the rational combination of parameters (c + \sum a_i y_i)/m
 * is always integral.  We do this by introducing an integer division
 * q = floor((c + \sum a_i y_i)/m) and stipulating that its argument should
 * always be integral in the context tableau, i.e., m q = c + \sum a_i y_i.
 * Since q is expressed in the tableau as
 *	c + \sum a_i y_i - m q >= 0
 *	-c - \sum a_i y_i + m q + m - 1 >= 0
 * it is sufficient to add the inequality
 *	-c - \sum a_i y_i + m q >= 0
 * In the part of the context where this inequality does not hold, the
 * main tableau is marked as being empty.
 */
static struct isl_sol *find_solutions(struct isl_sol *sol, struct isl_tab *tab)
{
	struct isl_tab **context_tab;

	if (!tab || !sol)
		goto error;

	context_tab = &sol->context_tab;

	if (tab->empty)
		goto done;
	if ((*context_tab)->empty)
		goto done;

	for (; tab && !tab->empty; tab = restore_lexmin(tab)) {
		int flags;
		int row;
		int sgn;
		int split = -1;
		int n_split = 0;

		for (row = tab->n_redundant; row < tab->n_row; ++row) {
			if (!isl_tab_var_from_row(tab, row)->is_nonneg)
				continue;
			sgn = row_sign(tab, sol, row);
			if (!sgn)
				goto error;
			tab->row_sign[row] = sgn;
			if (sgn == isl_tab_row_any)
				n_split++;
			if (sgn == isl_tab_row_any && split == -1)
				split = row;
			if (sgn == isl_tab_row_neg)
				break;
		}
		if (row < tab->n_row)
			continue;
		if (split != -1) {
			struct isl_vec *ineq;
			if (n_split != 1)
				split = best_split(tab, *context_tab);
			if (split < 0)
				goto error;
			ineq = get_row_parameter_ineq(tab, split);
			if (!ineq)
				goto error;
			is_strict(ineq);
			for (row = tab->n_redundant; row < tab->n_row; ++row) {
				if (!isl_tab_var_from_row(tab, row)->is_nonneg)
					continue;
				if (tab->row_sign[row] == isl_tab_row_any)
					tab->row_sign[row] = isl_tab_row_unknown;
			}
			tab->row_sign[split] = isl_tab_row_pos;
			sol = find_in_pos(sol, tab, ineq->el);
			tab->row_sign[split] = isl_tab_row_neg;
			row = split;
			isl_seq_neg(ineq->el, ineq->el, ineq->size);
			isl_int_sub_ui(ineq->el[0], ineq->el[0], 1);
			*context_tab = add_lexmin_ineq(*context_tab, ineq->el);
			*context_tab = check_samples(*context_tab, ineq->el, 0);
			isl_vec_free(ineq);
			if (!sol)
				goto error;
			continue;
		}
		if (tab->rational)
			break;
		row = first_non_integer(tab, &flags);
		if (row < 0)
			break;
		if (ISL_FL_ISSET(flags, I_PAR)) {
			if (ISL_FL_ISSET(flags, I_VAR)) {
				tab = isl_tab_mark_empty(tab);
				break;
			}
			row = add_cut(tab, row);
		} else if (ISL_FL_ISSET(flags, I_VAR)) {
			struct isl_vec *div;
			struct isl_vec *ineq;
			int d;
			if (isl_tab_extend_cons(*context_tab, 3) < 0)
				goto error;
			div = get_row_split_div(tab, row);
			if (!div)
				goto error;
			d = get_div(tab, context_tab, div);
			isl_vec_free(div);
			if (d < 0)
				goto error;
			ineq = ineq_for_div((*context_tab)->bset, d);
			sol = no_sol_in_strict(sol, tab, ineq);
			isl_seq_neg(ineq->el, ineq->el, ineq->size);
			*context_tab = add_lexmin_ineq(*context_tab, ineq->el);
			*context_tab = check_samples(*context_tab, ineq->el, 0);
			isl_vec_free(ineq);
			if (!sol)
				goto error;
			tab = set_row_cst_to_div(tab, row, d);
		} else
			row = add_parametric_cut(tab, row, context_tab);
		if (row < 0)
			goto error;
	}
done:
	sol = sol->add(sol, tab);
	isl_tab_free(tab);
	return sol;
error:
	isl_tab_free(tab);
	sol_free(sol);
	return NULL;
}

/* Compute the lexicographic minimum of the set represented by the main
 * tableau "tab" within the context "sol->context_tab".
 *
 * As a preprocessing step, we first transfer all the purely parametric
 * equalities from the main tableau to the context tableau, i.e.,
 * parameters that have been pivoted to a row.
 * These equalities are ignored by the main algorithm, because the
 * corresponding rows may not be marked as being non-negative.
 * In parts of the context where the added equality does not hold,
 * the main tableau is marked as being empty.
 */
static struct isl_sol *find_solutions_main(struct isl_sol *sol,
	struct isl_tab *tab)
{
	int row;

	for (row = tab->n_redundant; row < tab->n_row; ++row) {
		int p;
		struct isl_vec *eq;

		if (tab->row_var[row] < 0)
			continue;
		if (tab->row_var[row] >= tab->n_param &&
		    tab->row_var[row] < tab->n_var - tab->n_div)
			continue;
		if (tab->row_var[row] < tab->n_param)
			p = tab->row_var[row];
		else
			p = tab->row_var[row]
				+ tab->n_param - (tab->n_var - tab->n_div);

		if (isl_tab_extend_cons(sol->context_tab, 2) < 0)
			goto error;

		eq = isl_vec_alloc(tab->mat->ctx, 1+tab->n_param+tab->n_div);
		get_row_parameter_line(tab, row, eq->el);
		isl_int_neg(eq->el[1 + p], tab->mat->row[row][0]);
		eq = isl_vec_normalize(eq);

		sol = no_sol_in_strict(sol, tab, eq);

		isl_seq_neg(eq->el, eq->el, eq->size);
		sol = no_sol_in_strict(sol, tab, eq);
		isl_seq_neg(eq->el, eq->el, eq->size);

		sol->context_tab = add_lexmin_eq(sol->context_tab, eq->el);
		context_valid_sample_or_feasible(sol, eq->el, 1);
		sol->context_tab = check_samples(sol->context_tab, eq->el, 1);

		isl_vec_free(eq);

		isl_tab_mark_redundant(tab, row);

		if (!sol->context_tab)
			goto error;
		if (sol->context_tab->empty)
			break;

		row = tab->n_redundant - 1;
	}

	return find_solutions(sol, tab);
error:
	isl_tab_free(tab);
	sol_free(sol);
	return NULL;
}

static struct isl_sol_map *sol_map_find_solutions(struct isl_sol_map *sol_map,
	struct isl_tab *tab)
{
	return (struct isl_sol_map *)find_solutions_main(&sol_map->sol, tab);
}

/* Check if integer division "div" of "dom" also occurs in "bmap".
 * If so, return its position within the divs.
 * If not, return -1.
 */
static int find_context_div(struct isl_basic_map *bmap,
	struct isl_basic_set *dom, unsigned div)
{
	int i;
	unsigned b_dim = isl_dim_total(bmap->dim);
	unsigned d_dim = isl_dim_total(dom->dim);

	if (isl_int_is_zero(dom->div[div][0]))
		return -1;
	if (isl_seq_first_non_zero(dom->div[div] + 2 + d_dim, dom->n_div) != -1)
		return -1;

	for (i = 0; i < bmap->n_div; ++i) {
		if (isl_int_is_zero(bmap->div[i][0]))
			continue;
		if (isl_seq_first_non_zero(bmap->div[i] + 2 + d_dim,
					   (b_dim - d_dim) + bmap->n_div) != -1)
			continue;
		if (isl_seq_eq(bmap->div[i], dom->div[div], 2 + d_dim))
			return i;
	}
	return -1;
}

/* The correspondence between the variables in the main tableau,
 * the context tableau, and the input map and domain is as follows.
 * The first n_param and the last n_div variables of the main tableau
 * form the variables of the context tableau.
 * In the basic map, these n_param variables correspond to the
 * parameters and the input dimensions.  In the domain, they correspond
 * to the parameters and the set dimensions.
 * The n_div variables correspond to the integer divisions in the domain.
 * To ensure that everything lines up, we may need to copy some of the
 * integer divisions of the domain to the map.  These have to be placed
 * in the same order as those in the context and they have to be placed
 * after any other integer divisions that the map may have.
 * This function performs the required reordering.
 */
static struct isl_basic_map *align_context_divs(struct isl_basic_map *bmap,
	struct isl_basic_set *dom)
{
	int i;
	int common = 0;
	int other;

	for (i = 0; i < dom->n_div; ++i)
		if (find_context_div(bmap, dom, i) != -1)
			common++;
	other = bmap->n_div - common;
	if (dom->n_div - common > 0) {
		bmap = isl_basic_map_extend_dim(bmap, isl_dim_copy(bmap->dim),
				dom->n_div - common, 0, 0);
		if (!bmap)
			return NULL;
	}
	for (i = 0; i < dom->n_div; ++i) {
		int pos = find_context_div(bmap, dom, i);
		if (pos < 0) {
			pos = isl_basic_map_alloc_div(bmap);
			if (pos < 0)
				goto error;
			isl_int_set_si(bmap->div[pos][0], 0);
		}
		if (pos != other + i)
			isl_basic_map_swap_div(bmap, pos, other + i);
	}
	return bmap;
error:
	isl_basic_map_free(bmap);
	return NULL;
}

/* Compute the lexicographic minimum (or maximum if "max" is set)
 * of "bmap" over the domain "dom" and return the result as a map.
 * If "empty" is not NULL, then *empty is assigned a set that
 * contains those parts of the domain where there is no solution.
 * If "bmap" is marked as rational (ISL_BASIC_MAP_RATIONAL),
 * then we compute the rational optimum.  Otherwise, we compute
 * the integral optimum.
 *
 * We perform some preprocessing.  As the PILP solver does not
 * handle implicit equalities very well, we first make sure all
 * the equalities are explicitly available.
 * We also make sure the divs in the domain are properly order,
 * because they will be added one by one in the given order
 * during the construction of the solution map.
 */
struct isl_map *isl_tab_basic_map_partial_lexopt(
		struct isl_basic_map *bmap, struct isl_basic_set *dom,
		struct isl_set **empty, int max)
{
	struct isl_tab *tab;
	struct isl_map *result = NULL;
	struct isl_sol_map *sol_map = NULL;

	if (empty)
		*empty = NULL;
	if (!bmap || !dom)
		goto error;

	isl_assert(bmap->ctx,
	    isl_basic_map_compatible_domain(bmap, dom), goto error);

	bmap = isl_basic_map_detect_equalities(bmap);

	if (dom->n_div) {
		dom = isl_basic_set_order_divs(dom);
		bmap = align_context_divs(bmap, dom);
	}
	sol_map = sol_map_init(bmap, dom, !!empty, max);
	if (!sol_map)
		goto error;

	if (isl_basic_set_fast_is_empty(sol_map->sol.context_tab->bset))
		/* nothing */;
	else if (isl_basic_map_fast_is_empty(bmap))
		sol_map = add_empty(sol_map);
	else {
		tab = tab_for_lexmin(bmap,
					sol_map->sol.context_tab->bset, 1, max);
		tab = tab_detect_nonnegative_parameters(tab,
						sol_map->sol.context_tab);
		sol_map = sol_map_find_solutions(sol_map, tab);
		if (!sol_map)
			goto error;
	}

	result = isl_map_copy(sol_map->map);
	if (empty)
		*empty = isl_set_copy(sol_map->empty);
	sol_map_free(sol_map);
	isl_basic_map_free(bmap);
	return result;
error:
	sol_map_free(sol_map);
	isl_basic_map_free(bmap);
	return NULL;
}

struct isl_sol_for {
	struct isl_sol	sol;
	int		(*fn)(__isl_take isl_basic_set *dom,
				__isl_take isl_mat *map, void *user);
	void		*user;
	int		max;
};

static void sol_for_free(struct isl_sol_for *sol_for)
{
	isl_tab_free(sol_for->sol.context_tab);
	free(sol_for);
}

static void sol_for_free_wrap(struct isl_sol *sol)
{
	sol_for_free((struct isl_sol_for *)sol);
}

/* Add the solution identified by the tableau and the context tableau.
 *
 * See documentation of sol_map_add for more details.
 *
 * Instead of constructing a basic map, this function calls a user
 * defined function with the current context as a basic set and
 * an affine matrix reprenting the relation between the input and output.
 * The number of rows in this matrix is equal to one plus the number
 * of output variables.  The number of columns is equal to one plus
 * the total dimension of the context, i.e., the number of parameters,
 * input variables and divs.  Since some of the columns in the matrix
 * may refer to the divs, the basic set is not simplified.
 * (Simplification may reorder or remove divs.)
 */
static struct isl_sol_for *sol_for_add(struct isl_sol_for *sol,
	struct isl_tab *tab)
{
	struct isl_tab *context_tab;
	struct isl_basic_set *bset;
	struct isl_mat *mat = NULL;
	unsigned n_out;
	unsigned off;
	int row, i;

	if (!sol || !tab)
		goto error;

	if (tab->empty)
		return sol;

	off = 2 + tab->M;
	context_tab = sol->sol.context_tab;

	n_out = tab->n_var - tab->n_param - tab->n_div;
	mat = isl_mat_alloc(tab->mat->ctx, 1 + n_out, 1 + tab->n_param + tab->n_div);
	if (!mat)
		goto error;

	isl_seq_clr(mat->row[0] + 1, mat->n_col - 1);
	isl_int_set_si(mat->row[0][0], 1);
	for (row = 0; row < n_out; ++row) {
		int i = tab->n_param + row;
		int r, j;

		isl_seq_clr(mat->row[1 + row], mat->n_col);
		if (!tab->var[i].is_row)
			continue;

		r = tab->var[i].index;
		/* no unbounded */
		if (tab->M)
			isl_assert(mat->ctx, isl_int_eq(tab->mat->row[r][2],
					                tab->mat->row[r][0]),
				    goto error);
		isl_int_set(mat->row[1 + row][0], tab->mat->row[r][1]);
		for (j = 0; j < tab->n_param; ++j) {
			int col;
			if (tab->var[j].is_row)
				continue;
			col = tab->var[j].index;
			isl_int_set(mat->row[1 + row][1 + j],
				    tab->mat->row[r][off + col]);
		}
		for (j = 0; j < tab->n_div; ++j) {
			int col;
			if (tab->var[tab->n_var - tab->n_div+j].is_row)
				continue;
			col = tab->var[tab->n_var - tab->n_div+j].index;
			isl_int_set(mat->row[1 + row][1 + tab->n_param + j],
				    tab->mat->row[r][off + col]);
		}
		if (!isl_int_is_one(tab->mat->row[r][0]))
			isl_seq_scale_down(mat->row[1 + row], mat->row[1 + row],
					    tab->mat->row[r][0], mat->n_col);
		if (sol->max)
			isl_seq_neg(mat->row[1 + row], mat->row[1 + row],
				    mat->n_col);
	}

	bset = isl_basic_set_dup(context_tab->bset);
	bset = isl_basic_set_finalize(bset);

	if (sol->fn(bset, isl_mat_copy(mat), sol->user) < 0)
		goto error;

	isl_mat_free(mat);
	return sol;
error:
	isl_mat_free(mat);
	sol_free(&sol->sol);
	return NULL;
}

static struct isl_sol *sol_for_add_wrap(struct isl_sol *sol,
	struct isl_tab *tab)
{
	return (struct isl_sol *)sol_for_add((struct isl_sol_for *)sol, tab);
}

static struct isl_sol_for *sol_for_init(struct isl_basic_map *bmap, int max,
	int (*fn)(__isl_take isl_basic_set *dom, __isl_take isl_mat *map,
		  void *user),
	void *user)
{
	struct isl_sol_for *sol_for = NULL;
	struct isl_dim *dom_dim;
	struct isl_basic_set *dom = NULL;
	struct isl_tab *context_tab;
	int f;

	sol_for = isl_calloc_type(bset->ctx, struct isl_sol_for);
	if (!sol_for)
		goto error;

	dom_dim = isl_dim_domain(isl_dim_copy(bmap->dim));
	dom = isl_basic_set_universe(dom_dim);

	sol_for->fn = fn;
	sol_for->user = user;
	sol_for->max = max;
	sol_for->sol.add = &sol_for_add_wrap;
	sol_for->sol.free = &sol_for_free_wrap;

	context_tab = context_tab_for_lexmin(isl_basic_set_copy(dom));
	context_tab = restore_lexmin(context_tab);
	sol_for->sol.context_tab = context_tab;
	f = context_is_feasible(&sol_for->sol);
	if (f < 0)
		goto error;

	isl_basic_set_free(dom);
	return sol_for;
error:
	isl_basic_set_free(dom);
	sol_for_free(sol_for);
	return NULL;
}

static struct isl_sol_for *sol_for_find_solutions(struct isl_sol_for *sol_for,
	struct isl_tab *tab)
{
	return (struct isl_sol_for *)find_solutions_main(&sol_for->sol, tab);
}

int isl_basic_map_foreach_lexopt(__isl_keep isl_basic_map *bmap, int max,
	int (*fn)(__isl_take isl_basic_set *dom, __isl_take isl_mat *map,
		  void *user),
	void *user)
{
	struct isl_sol_for *sol_for = NULL;

	bmap = isl_basic_map_copy(bmap);
	if (!bmap)
		return -1;

	bmap = isl_basic_map_detect_equalities(bmap);
	sol_for = sol_for_init(bmap, max, fn, user);

	if (isl_basic_map_fast_is_empty(bmap))
		/* nothing */;
	else {
		struct isl_tab *tab;
		tab = tab_for_lexmin(bmap,
					sol_for->sol.context_tab->bset, 1, max);
		tab = tab_detect_nonnegative_parameters(tab,
						sol_for->sol.context_tab);
		sol_for = sol_for_find_solutions(sol_for, tab);
		if (!sol_for)
			goto error;
	}

	sol_for_free(sol_for);
	isl_basic_map_free(bmap);
	return 0;
error:
	sol_for_free(sol_for);
	isl_basic_map_free(bmap);
	return -1;
}

int isl_basic_map_foreach_lexmin(__isl_keep isl_basic_map *bmap,
	int (*fn)(__isl_take isl_basic_set *dom, __isl_take isl_mat *map,
		  void *user),
	void *user)
{
	return isl_basic_map_foreach_lexopt(bmap, 0, fn, user);
}

int isl_basic_map_foreach_lexmax(__isl_keep isl_basic_map *bmap,
	int (*fn)(__isl_take isl_basic_set *dom, __isl_take isl_mat *map,
		  void *user),
	void *user)
{
	return isl_basic_map_foreach_lexopt(bmap, 1, fn, user);
}
