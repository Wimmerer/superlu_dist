/*! \file
Copyright (c) 2003, The Regents of the University of California, through
Lawrence Berkeley National Laboratory (subject to receipt of any required
approvals from U.S. Dept. of Energy)

All rights reserved.

The source code is distributed under BSD license, see the file License.txt
at the top-level directory.
*/


/*! @file
 * \brief Computes row and column scalings, restricting the scale factors to be power-of-radix
 *
 * File name: psgsequb.c
 * History:   similar to LAPACK routine SGEEQUB
 */
#include <math.h>
#include "superlu_sdefs.h"

/*! \brief

 <pre>
    Purpose
    =======

    PSGSEQUB computes row and column scalings intended to equilibrate an
    M-by-N sparse matrix A and reduce its condition number. R returns the row
    scale factors and C the column scale factors, chosen to try to make
    the largest element in each row and column of the matrix B with
    elements B(i,j)=R(i)*A(i,j)*C(j) have absolute value 1.

    R(i) and C(j) are restricted to be a power-of-radix between 
    SMLNUM = smallest safe number and BIGNUM = largest safe number.
    Use of these scaling factors is not guaranteed to reduce the 
    condition number of A but works well in practice.

    This routine differs from PSGSEQU by restricting the scaling factors                       
    to a power-of-radix.  Barring over- and underflow, scaling by                           
    these factors introduces no additional rounding errors.  However,
    the scaled entries' magnitudes are no longer approximately 1 but lie                            
    between sqrt(radix) and 1/sqrt(radix).

    See supermatrix.h for the definition of 'SuperMatrix' structure.

    Arguments
    =========

    A       (input) SuperMatrix*
            The matrix of dimension (A->nrow, A->ncol) whose equilibration
            factors are to be computed. The type of A can be:
            Stype = SLU_NR_loc; Dtype = SLU_S; Mtype = SLU_GE.

    R       (output) float*, size A->nrow
            If INFO = 0 or INFO > M, R contains the row scale factors
            for A.

    C       (output) float*, size A->ncol
            If INFO = 0,  C contains the column scale factors for A.

    ROWCND  (output) float*
            If INFO = 0 or INFO > M, ROWCND contains the ratio of the
            smallest R(i) to the largest R(i).  If ROWCND >= 0.1 and
            AMAX is neither too large nor too small, it is not worth
            scaling by R.

    COLCND  (output) float*
            If INFO = 0, COLCND contains the ratio of the smallest
            C(i) to the largest C(i).  If COLCND >= 0.1, it is not
            worth scaling by C.

    AMAX    (output) float*
            Absolute value of largest matrix element.  If AMAX is very
            close to overflow or very close to underflow, the matrix
            should be scaled.

    INFO    (output) int*
            = 0:  successful exit
            < 0:  if INFO = -i, the i-th argument had an illegal value
            > 0:  if INFO = i,  and i is
                  <= M:  the i-th row of A is exactly zero
                  >  M:  the (i-M)-th column of A is exactly zero

    GRID    (input) gridinof_t*
            The 2D process mesh.
    =====================================================================
</pre>
*/

void
psgsequb(SuperMatrix *A, float *r, float *c, float *rowcnd,
	 float *colcnd, float *amax, int_t *info, gridinfo_t *grid)
{

    /* Local variables */
    NRformat_loc *Astore;
    float *Aval;
    int_t i, j, irow, jcol, m_loc, iexp;
    float rcmin, rcmax;
    float bignum, smlnum, radix;
    float tempmax, tempmin;
    float *loc_max;
    int *r_sizes, *displs;
    float *loc_r;
    int_t  procs;

    /* Test the input parameters. */
    *info = 0;
    if ( A->nrow < 0 || A->ncol < 0 ||
	 A->Stype != SLU_NR_loc || A->Dtype != SLU_S || A->Mtype != SLU_GE )
	*info = -1;
    if (*info != 0) {
	i = -(*info);
	pxerr_dist("psgsequ", grid, i);
	return;
    }

    /* Quick return if possible */
    if ( A->nrow == 0 || A->ncol == 0 ) {
	*rowcnd = 1.;
	*colcnd = 1.;
	*amax = 0.;
	return;
    }

    Astore = A->Store;
    Aval = Astore->nzval;
    m_loc = Astore->m_loc;

    /* Get machine constants. */
    smlnum = smach_dist("S");
    bignum = 1. / smlnum;
    radix = smach_dist("B");

    /* Compute row scale factors. */
    for (i = 0; i < A->nrow; ++i) r[i] = 0.;

    /* Find the maximum element in each row. */
    irow = Astore->fst_row;
    for (i = 0; i < m_loc; ++i) {
        for (j = Astore->rowptr[i]; j < Astore->rowptr[i+1]; ++j)
	    r[irow] = SUPERLU_MAX( r[irow], fabs(Aval[j]) );
	if (r[irow] > 0.) { /* make it power-of-radix */
	  iexp = (int) log2(r[irow])/log2(radix);
	  r[irow] = pow( (double)radix, (double)iexp );
	  //if ( grid->iam==0 ) printf("iexp %d, r[%d] %f\n", iexp, irow, r[irow]);	  
	}
	++irow;
    }

    /* Find the maximum and minimum scale factors. */
    rcmin = bignum;
    rcmax = 0.;
    for (i = Astore->fst_row; i < Astore->fst_row + m_loc; ++i) {
	rcmax = SUPERLU_MAX(rcmax, r[i]);
	rcmin = SUPERLU_MIN(rcmin, r[i]);
    }

    /* Get the global MAX and MIN for R */
    tempmax = rcmax;
    tempmin = rcmin;
    MPI_Allreduce( &tempmax, &rcmax,
		1, MPI_FLOAT, MPI_MAX, grid->comm);
    MPI_Allreduce( &tempmin, &rcmin,
		1, MPI_FLOAT, MPI_MIN, grid->comm);

    *amax = rcmax;
#if ( PRNTlevel>=1 )    
    if ( grid->iam == 0) {
      printf(".. psgsequb: rmax %.16f, rmin %.16f, amax %.16f\n", rcmax, rcmin, *amax);
      fflush(stdout);
    }
#endif
    
    if (rcmin == 0.) {
	/* Find the first zero scale factor and return an error code. */
	for (i = 0; i < A->nrow; ++i)
	    if (r[i] == 0.) {
		*info = i + 1;
		return;
	    }
    } else {
	/* Invert the scale factors. */
	for (i = 0; i < A->nrow; ++i)
	    r[i] = 1. / SUPERLU_MIN( SUPERLU_MAX( r[i], smlnum ), bignum );
	/* Compute ROWCND = min(R(I)) / max(R(I)) */
	*rowcnd = SUPERLU_MAX( rcmin, smlnum ) / SUPERLU_MIN( rcmax, bignum );
    }

    /* Compute column scale factors */
    for (j = 0; j < A->ncol; ++j) c[j] = 0.;

    /* Find the maximum element in each column, assuming the row
       scalings computed above. */
    irow = Astore->fst_row;
    for (i = 0; i < m_loc; ++i) {
        for (j = Astore->rowptr[i]; j < Astore->rowptr[i+1]; ++j) {
	    jcol = Astore->colind[j];
	    c[jcol] = SUPERLU_MAX( c[jcol], fabs(Aval[j]) * r[irow] );
	}
	++irow;
    }

    /* Find the global maximum for c[j] */
    if ( !(loc_max = floatMalloc_dist(A->ncol)))
      ABORT("Malloc fails for loc_max[].");
    for (j = 0; j < A->ncol; ++j) {
	if (c[j] > 0.) { /* make it power-of-radix */
	  iexp = (int) log2(c[j])/log2(radix);
	  c[j] = pow( (double)radix, (double)iexp );
	}
	loc_max[j] = c[j];
    }
    MPI_Allreduce(loc_max, c, A->ncol, MPI_FLOAT, MPI_MAX, grid->comm);
    SUPERLU_FREE(loc_max);

    /* Find the maximum and minimum scale factors. */
    rcmin = bignum;
    rcmax = 0.;
    for (j = 0; j < A->ncol; ++j) {
	rcmax = SUPERLU_MAX(rcmax, c[j]);
	rcmin = SUPERLU_MIN(rcmin, c[j]);
    }

#if ( PRNTlevel>=1 )    
    if ( grid->iam == 0) {
      printf(".. psgsequb: cmax %f, cmin %f\n", rcmax, rcmin);
      fflush(stdout);
    }
#endif
    
    if (rcmin == 0.) {
	/* Find the first zero scale factor and return an error code. */
	for (j = 0; j < A->ncol; ++j)
	    if ( c[j] == 0. ) {
		*info = A->nrow + j + 1;
		return;
	    }
    } else {
	/* Invert the scale factors. */
	for (j = 0; j < A->ncol; ++j)
	    c[j] = 1. / SUPERLU_MIN( SUPERLU_MAX( c[j], smlnum ), bignum);
	/* Compute COLCND = min(C(J)) / max(C(J)) */
	*colcnd = SUPERLU_MAX( rcmin, smlnum ) / SUPERLU_MIN( rcmax, bignum );
    }

    /* gather R from each process to get the global R.  */

    procs = grid->nprow * grid->npcol;
    if ( !(r_sizes = SUPERLU_MALLOC(2 * procs * sizeof(int))))
      ABORT("Malloc fails for r_sizes[].");
    displs = r_sizes + procs;
    if ( !(loc_r = floatMalloc_dist(m_loc)))
      ABORT("Malloc fails for loc_r[].");
    j = Astore->fst_row;
    for (i = 0; i < m_loc; ++i) loc_r[i] = r[j++];

    /* First gather the size of each piece. */
    MPI_Allgather(&m_loc, 1, MPI_INT, r_sizes, 1, MPI_INT, grid->comm);

    /* Set up the displacements for allgatherv */
    displs[0] = 0;
    for (i = 1; i < procs; ++i) displs[i] = displs[i-1] + r_sizes[i-1];

    /* Now gather the actual data */
    MPI_Allgatherv(loc_r, m_loc, MPI_FLOAT, r, r_sizes, displs,
		   MPI_FLOAT, grid->comm);

    SUPERLU_FREE(r_sizes);
    SUPERLU_FREE(loc_r);

    return;

} /* end psgsequb */
