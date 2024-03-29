/*
** Code to implement a d2q9-bgk lattice boltzmann scheme.
** 'd2' inidates a 2-dimensional grid, and
** 'q9' indicates 9 velocities per grid cell.
** 'bgk' refers to the Bhatnagar-Gross-Krook collision step.
**
** The 'speeds' in each cell are numbered as follows:
**
** 6 2 5
**  \|/
** 3-0-1
**  /|\
** 7 4 8
**
** A 2D grid:
**
**           cols
**       --- --- ---
**      | D | E | F |
** rows  --- --- ---
**      | A | B | C |
**       --- --- ---
**
** 'unwrapped' in row major order to give a 1D array:
**
**  --- --- --- --- --- ---
** | A | B | C | D | E | F |
**  --- --- --- --- --- ---
**
** Grid indices are:
**
**          ny
**          ^       cols(ii)
**          |  ----- ----- -----
**          | | ... | ... | etc |
**          |  ----- ----- -----
** rows(jj) | | 1,0 | 1,1 | 1,2 |
**          |  ----- ----- -----
**          | | 0,0 | 0,1 | 0,2 |
**          |  ----- ----- -----
**          ----------------------> nx
**
** Note the names of the input parameter and obstacle files
** are passed on the command line, e.g.:
**
**   ./d2q9-bgk input.params obstacles.dat
**
** Be sure to adjust the grid dimensions in the parameter file
** if you choose a different obstacle file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h> /* angle brackets: standard library header file (searches dirs pre-designated by compiler/IDE first) */
#include "mpi.h"          /* quotes: programmer-defined header file (searches this dir first, then same as <>) */

/* define debug variables */
/* #define DEBUG                    included */
/* #define DEBUG_localNy            prints local_ny var: no. of cells in y-direction in decomposed grid */
/* #define DEBUG_mainGridV          prints main grid (*cells_ptr) values after initialisation */
/* #define DEBUG_mainGrid           prints only main grid (*cells_ptr) size after initialisation */
/* #define DEBUG_obstacleGrid       prints obstacle grid (*obstacles_ptr) values */
/* #define DEBUG_init_checkpoints   prints checkpoints during initialise() execution */
/* #define DEBUG_ranks_updn         prints ranks above & below current rank */
 #define DEBUG_state_timestep    /* prints params, cells, tmp_cells, obstacles, and obstacles_total in timestep */

/* macro to get size of an array */
#define NELEMS(x)  (sizeof(x) / sizeof((x)[0]))

#define NSPEEDS         9
/* output files for error checking in check.py */
#define FINALSTATEFILE  "final_state.dat"
#define AVVELSFILE      "av_vels.dat"

/* struct to hold the parameter values */
typedef struct
{
  int   nx;           /* no. of cells in x-direction */
  int   ny;           /* no. of cells in y-direction */
  int   maxIters;     /* no. of iterations */
  int   reynolds_dim; /* dimension for Reynolds number */
  float density;      /* density per link */
  float accel;        /* density redistribution */
  float omega;        /* relaxation parameter */
} t_param;            /* typedef allows referencing without struct keyword */

/* struct to hold the 'speed' values */
typedef struct
{
  float speeds[NSPEEDS];
} t_speed;

/*
** function prototypes
*/

/* load params, allocate memory, load obstacles & initialise fluid particle densities */
int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr_total, int** obstacles_ptr,
               float** av_vels_ptr, int size, float** send_buff_up,
               float** send_buff_dn, float** recv_buff_up, float** recv_buff_dn);

/*
** The main calculation methods.
** timestep calls, in order, the functions:
** accelerate_flow(), propagate(), rebound() & collision()
*/
int timestep(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);

int accelerate_flow(const t_param params, t_speed* cells, int* obstacles);
int propagate(const t_param params, t_speed* cells, t_speed* tmp_cells);
int rebound(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);
int collision(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles);

/* compute average velocity */
float av_velocity(const t_param params, t_speed* cells, int* obstacles);

/* Sum all the densities in the grid.
** The total should remain constant from one timestep to the next. */
float total_density(const t_param params, t_speed* cells);

/* calculate Reynolds number */
float calc_reynolds(const t_param params, t_speed* cells, int* obstacles);

int write_values(const t_param params, t_speed* cells, int* obstacles, float* av_vels);

/* finalise, including freeing up allocated memory */
int finalise(const t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
             int** obstacles_ptr, float** av_vels_ptr);

/* utility functions */
void die(const char* message, const int line, const char* file);
void usage(const char* exe);

/*
** main program:
** initialise, timestep loop, finalise
*/
int main(int argc, char* argv[])
{
  char*    paramfile = NULL;    /* name of the input parameter file */
  char*    obstaclefile = NULL; /* name of a the input obstacle file */
  t_param  params;              /* struct to hold parameter values */
  t_speed* cells     = NULL;    /* grid containing fluid densities */
  t_speed* tmp_cells = NULL;    /* scratch space */
  int*     obstacles = NULL;    /* grid indicating which cells are blocked (local) */
  float* av_vels     = NULL;    /* a record of the av. velocity computed for each timestep */
  struct timeval timstr;        /* structure to hold elapsed time */
  struct rusage ru;             /* structure to hold CPU time--system and user */
  double tic, toc;              /* floating point numbers to calculate elapsed wallclock time */
  double usrtim;                /* floating point number to record elapsed user CPU time */
  double systim;                /* floating point number to record elapsed system CPU time */

  /* MPI vars */
  int flag;         /* for checking whether MPI_Init() has been called */
  int rank;         /* 'rank' of process among it's cohort */ 
  int size;         /* size of cohort, i.e. num processes started */
  int up;           /* rank of process above current one */
  int dn;           /* rank of process below current one */
  int* obstacles_total = NULL;  /* grid indicating which cells are blocked (total) */
  float* send_buff_up  = NULL;  /* send/receive buffers for halo exchange */
  float* send_buff_dn  = NULL;
  float* recv_buff_up  = NULL;
  float* recv_buff_dn  = NULL;

  /* MPI constants */
  #define MASTER 0

  /* parse the command line */
  if (argc != 3)
  {
    usage(argv[0]);
  }
  else
  {
    paramfile = argv[1];
    obstaclefile = argv[2];
  }

  /* Initialise our MPI environment */
  MPI_Init(&argc, &argv);
  MPI_Initialized(&flag);
  if (flag != 1) {
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }

  /* 
  ** determine the SIZE of the group of processes associated with
  ** the 'communicator'.  MPI_COMM_WORLD is the default communicator
  ** consisting of all the processes in the launched MPI 'job'
  */
  MPI_Comm_size( MPI_COMM_WORLD, &size );
  
  /* determine the RANK of the current process [0:SIZE-1] */
  MPI_Comm_rank( MPI_COMM_WORLD, &rank );

  /* initialise our data structures and load values from file */
  initialise(paramfile, obstaclefile, &params, &cells, &tmp_cells,
             &obstacles_total, &obstacles, &av_vels, size, &send_buff_up,
             &send_buff_dn, &recv_buff_up, &recv_buff_dn);

  printf("\n\n\nINITIALISATION SUCCESSFUL\n\n\n");

  /*
  ** determine process ranks above and below this rank
  ** respecting periodic boundary conditions (rank + size -1 wrap around to bottom rank)
  */
  up = (rank == MASTER) ? (rank + size - 1) : (rank - 1);
  dn = (rank + 1) % size;
  #ifdef DEBUG_ranks_updn
  printf("Rank: %d Above: %d Below: %d\n", rank, up, dn);
  #endif

  /* determine local grid size??? */

  printf("\n\n\nINITIALISATION SUCCESSFUL\n\n\n");

  /* begin timing pre-execution */
  gettimeofday(&timstr, NULL);
  tic = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  /* iterate for maxIters timesteps */
  /* --------------------------------- MAIN LOOP --------------------------------- */
  for (int tt = 0; tt < params.maxIters; tt++)
  {
    if (tt == 0) {
      #ifdef DEBUG_state_timestep
      int aa, bb, cc, dd, ee, ff, gg, hh, ii, jj;
      int local_ny = params.ny / size;
      int count0 = 0;

      printf("Params: %d %d %d %d %.4f %.4f %.4f\n", params.nx, params.ny, params.maxIters,
      params.reynolds_dim, params.density, params.accel, params.omega);

      printf("Printing main grid:\n");
      for (aa = 0; aa < local_ny+2; aa++) {
        printf("\nRow %d\n", aa+1);
        for (bb = 0; bb < params.nx; bb++) {
          for (int cc = 0; cc < 9; cc++) {
            printf("%.2f ", cells[bb + aa].speeds[cc]);
            count0++;
          }
        }
        printf("%1cRow length: %d\n", ' ', bb);
      }
      printf("Main grid length: %d\n", count0);

      count0 = 0;
      printf("Printing helper grid:\n");
      for (dd = 0; dd < local_ny+2; dd++) {
        printf("\nRow %d\n", dd+1);
        for (ee = 0; ee < params.nx; ee++) {
          for (ff = 0; ff < 9; ff++) {
            printf("%.2f", tmp_cells[ee + dd].speeds[ff]);
            count0++;
          }
        }
        printf("%1cRow length: %d\n", ' ', ee);
      }
      printf("Helper grid length: %d\n", count0);

      count0 = 0;
      printf("Printing total obstacles:\n");
      for (gg = 0; gg < params.ny; gg++) {
        printf("\nRow %d\n", gg+1);
        for (hh = 0; hh < params.nx; hh++) {
          printf("%d", obstacles_total[hh + gg]);
          count0++;
        }
        printf("%1cRow length: %d\n", ' ', hh);
      }
      printf("Obstacle grid length: %d\n", count0);

      count0 = 0;
      printf("Printing local obstacles:\n");
      for (ii = 0; ii < local_ny; ii++) {
        printf("\nRow %d\n", ii+1);
        for (jj = 0; jj < params.nx; jj++) {
          printf("%d", obstacles[jj + ii]);
          count0++;
        }
        printf("%1cRow length: %d\n", ' ', jj);
      }
      printf("Local obstacle grid length: %d\n", count0);
      #endif
    }
    timestep(params, cells, tmp_cells, obstacles);
    av_vels[tt] = av_velocity(params, cells, obstacles);
    /* #ifdef DEBUG
    ** printf("==timestep: %d==\n", tt);
    ** printf("av velocity: %.12E\n", av_vels[tt]);
    ** printf("tot density: %.12E\n", total_density(params, cells));
    ** #endif */
  }

  /* MPI alterations
  ** halo exchange for local grids
  ** - send above, receive below
  ** - send below, receive above
  ** For each direction:
  ** - pack send buffer using grid values
  ** - exchange MPI_Sendrecv()
  ** unpack receive buffer into grid
  */
  /* ------------------------------- END MAIN LOOP ------------------------------- */

  /* calculate timing post-execution */
  gettimeofday(&timstr, NULL);
  toc = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  getrusage(RUSAGE_SELF, &ru);
  timstr = ru.ru_utime;
  usrtim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  timstr = ru.ru_stime;
  systim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  /*
  ** if (rank == MASTER) {
  **   GATHER DATA
  **   WRITE VALUES
  ** }
  */
  /*  
  ** Inverse MPI_Scatter
  ** Only root process needs to have valid receive buffer
  ** All other calling processes can pass NULL for recv_data
  ** MPI_Gather (
  ** void* send_data,
  ** int send_count
  ** MPI_Datatype send_datatype,
  ** void* recv_data,
  ** int recv_count,                // count of elements received per process, not total elements received
  ** MPI_Datatype recv_datatype,
  ** int root,
  ** MPI_Comm communicator
  ** )
  */

  /* write final values and free memory */
  printf("==done==\n");
  printf("Reynolds number:\t\t%.12E\n", calc_reynolds(params, cells, obstacles));
  printf("Elapsed time:\t\t\t%.6lf (s)\n", toc - tic);
  printf("Elapsed user CPU time:\t\t%.6lf (s)\n", usrtim);
  printf("Elapsed system CPU time:\t%.6lf (s)\n", systim);
  write_values(params, cells, obstacles, av_vels);
  finalise(&params, &cells, &tmp_cells, &obstacles, &av_vels);

  /* finalise the MPI environment */
  MPI_Finalize();
  MPI_Finalized(&flag);
  if(flag != 1) {
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }

  /* exit the program */
  return EXIT_SUCCESS;
}

int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr_total, int** obstacles_ptr,
               float** av_vels_ptr, int size, float** send_buff_up,
               float** send_buff_dn, float** recv_buff_up, float** recv_buff_dn)
{
  char   message[1024];  /* message buffer */
  FILE*  fp;             /* file pointer */
  int    xx, yy;         /* generic array indices */
  int    blocked;        /* indicates whether a cell is blocked by an obstacle */
  int    retval;         /* to hold return value for checking */

  /* MPI_vars */
  int local_ny;          /* no. of cells in y-direction in decomposed grid */

  #ifdef DEBUG_init_checkpoints
  printf("Initialisation begun\n\n\n");
  #endif

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }

  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1) die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1) die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1) die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);

  /*
  ** Allocate memory.
  **
  ** Remember C is pass-by-value, so we need to
  ** pass pointers into the initialise function.
  **
  ** NB we are allocating a 1D array, so that the
  ** memory will be contiguous.  We still want to
  ** index this memory as if it were a (row major
  ** ordered) 2D array, however.  We will perform
  ** some arithmetic using the row and column
  ** coordinates, inside the square brackets, when
  ** we want to access elements of this array.
  **
  ** Note also that we are using a structure to
  ** hold an array of 'speeds'.  We will allocate
  ** a 1D array of these structs.
  */

  /* change indexing for halo exchange
  ** - local grid (2 extra rows for halo exchange)
  ** - buffers for message passing
  */

  /* split rows by number of processors local_ny */
  local_ny = params->ny / size;
  #ifdef DEBUG_localNy
  printf("# of ranks in world: %d\n", size);
  printf("local_ny: no. of cells in y-direction in decomposed grid  %d\n", local_ny);
  #endif

  #ifdef DEBUG_init_checkpoints
  printf("Allocation beginning\n");
  #endif

  /* Main grid (w) */
  /* +2 to params->ny for halo rows... use local_ny */
  /* Main grid size = size of (no. of cells in y-direction * no. of cells in x-direction) * size of t_speed struct */
  *cells_ptr = (t_speed*)malloc(sizeof(t_speed) * ((local_ny + 2) * params->nx));

  if (*cells_ptr == NULL) die("cannot allocate memory for cells", __LINE__, __FILE__);

  /* Helper grid, used as scratch space (u) */
  /* +2 to params->ny for halo rows... use local_ny */
  /* Helper grid size = size of (no. of cells in y-direction * no. of cells in x-direction) * size of t_speed struct */
  *tmp_cells_ptr = (t_speed*)malloc(sizeof(t_speed) * ((local_ny + 2) * params->nx));

  if (*tmp_cells_ptr == NULL) die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  /* Obstacle map size = size of (no. of cells in y-direction * no. of cells in x-direction) * size of int */
  *obstacles_ptr_total = malloc(sizeof(int) * (params->ny * params->nx));

  if (*obstacles_ptr_total == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* use local_ny, but doesn't require +2 for halos */
  /* Local obstacle map size = size of (no. of cells in y-direction * no. of cells in x-direction) * size of int */
  *obstacles_ptr = malloc(sizeof(int) * (local_ny * params->nx));

  if (*obstacles_ptr == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* allocate space to hold a record of the avarage velocities computed at each timestep */
  *av_vels_ptr = (float*)malloc(sizeof(float) * params->maxIters);

  if (*av_vels_ptr == NULL) die("Cannot allocate memory for av_vels", __LINE__, __FILE__);

  /* allocate send & recv buffers */
  *send_buff_up = (float*)malloc(sizeof(float) * params->nx);
  *send_buff_dn = (float*)malloc(sizeof(float) * params->nx);
  *recv_buff_up = (float*)malloc(sizeof(float) * params->nx);
  *recv_buff_dn = (float*)malloc(sizeof(float) * params->nx);

  #ifdef DEBUG_init_checkpoints
  printf("Allocation complete\n");
  #endif

  /* change indexing for halo exchange
  ** - set boundary conditions
  ** - init inner cells to average of boundary cells???
  ** modify looping bounds to accommodate halo rows
  */

  /* initialise densities for present time (w) */
  float w0 = params->density * 4.f / 9.f;
  float w1 = params->density       / 9.f;
  float w2 = params->density       / 36.f;

  /* change loop boundaries ny -> local_ny */
  /* +1 to jj before adding with ii and multiplying, to account for top halo */
  /* change < to <=, to account for bottom halo */
  for (int jj = 1; jj <= local_ny; jj++)   /* row */
  {
    for (int ii = 0; ii < params->nx; ii++) /* cols */
    {
      /*
      ** 6 2 5
      **  \|/
      ** 3-0-1
      **  /|\
      ** 7 4 8
      */
      /* centre */
      (*cells_ptr)[ii + (jj)*params->nx].speeds[0] = w0;
      /* axis directions */
      (*cells_ptr)[ii + (jj)*params->nx].speeds[1] = w1;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[2] = w1;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[3] = w1;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[4] = w1;
      /* diagonals */
      (*cells_ptr)[ii + (jj)*params->nx].speeds[5] = w2;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[6] = w2;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[7] = w2;
      (*cells_ptr)[ii + (jj)*params->nx].speeds[8] = w2;
    }
  }

  #ifdef DEBUG_mainGrid
  int count = 0;
  printf("Printing main grid:\n");
  for (int jj = 0; jj < local_ny+2; jj++) {
    for (int ii = 0; ii < params->nx; ii++) {
      for (int ss = 0; ss < 9; ss++) {
        #ifdef DEBUG_mainGridV
        printf("%.2f", (*cells_ptr)[ii + jj].speeds[ss]);
        #endif
        count++;
      }
    }
  }
  printf("Main grid length should be 9 * N^2\n");
  printf("...OR 9 * (N+(size*2)) * N if halo is active\n");
  printf("Main grid length: %d\n", 4*count);
  #endif

  #ifdef DEBUG_init_checkpoints
  printf("Setting total to 0 beginning\n");
  #endif

  /* first set all cells in total obstacle array to zero */
  for (int jj = 0; jj < params->ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      (*obstacles_ptr_total)[ii + jj*params->nx] = 0;
    }
  }

  #ifdef DEBUG_init_checkpoints
  printf("Setting total to 0 complete\n\n");
  #endif

  #ifdef DEBUG_obstacleGrid
  int count2 = 0;
  printf("Printing obstacle grid:\n");
  for (int jj = 0; jj < local_ny; jj++) {
    for (int ii = 0; ii < params->nx; ii++) {
        printf("%d", (*obstacles_ptr)[ii + jj]);
        count2++;
    }
  }
  printf("Obstacle grid length: %d\n", count2);
  #endif

  #ifdef DEBUG_init_checkpoints
  int rank;
  MPI_Comm_rank( MPI_COMM_WORLD, &rank );
  printf("Rank %d Checkpoint0\n", rank);
  #endif

  /* open the obstacle data file */
  fp = fopen(obstaclefile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input obstacles file: %s", obstaclefile);
    die(message, __LINE__, __FILE__);
  }

  #ifdef DEBUG_init_checkpoints
  printf("Rank %d Checkpoint01\n", rank);
  #endif

  /* read-in the blocked cells list */
  int count = 0;
  while ((retval = fscanf(fp, "%d %d %d\n", &xx, &yy, &blocked)) != EOF)
  {
    /* some checks */
    if (retval != 3) die("expected 3 values per line in obstacle file", __LINE__, __FILE__);

    if (xx < 0 || xx > params->nx - 1) die("obstacle x-coord out of range", __LINE__, __FILE__);

    if (yy < 0 || yy > params->ny - 1) die("obstacle y-coord out of range", __LINE__, __FILE__);

    if (blocked != 1) die("obstacle blocked value should be 1", __LINE__, __FILE__);

    #ifdef DEBUG_init_checkpoints
    printf("Rank %d Checkpoint02. Count %d\n", rank, count);
    count++;
    #endif

    /* assign obstacle to the total obstacles array */
    (*obstacles_ptr_total)[xx + yy*params->nx] = blocked;
  }

  #ifdef DEBUG_init_checkpoints
  printf("Rank %d Checkpoint10\n", rank);
  #endif

  #ifdef DEBUG_init_checkpoints
  printf("Setting local to 0 beginning\n");
  #endif

  /* first set all cells in local obstacle array to appropriate region of total obstacle array */
  for (int jj = 0; jj < local_ny; jj++)
  {
    for (int ii = 0; ii < params->nx; ii++)
    {
      (*obstacles_ptr)[ii + jj*params->nx] = (*obstacles_ptr_total)[ii + jj*params->nx];
    }
  }

  #ifdef DEBUG_init_checkpoints
  printf("Setting local to 0 complete\n\n");
  #endif

  /* and close the file */
  fclose(fp);

  /* scatter arrays to other processes */
  /*  
  ** MPI_Scatter (
  ** void* send_data,                array of data residing on MASTER
  ** int send_count                  how many elements will be sent to each process? Often # elements in an array / num_proc
  ** MPI_Datatype send_datatype,     what MPI datatype will those elements be
  ** void* recv_data,                buffer holds recv_count number of elements of type recv_datatype
  ** int recv_count,
  ** MPI_Datatype recv_datatype,
  ** int root,                       root process scattering the array (MASTER)
  ** MPI_Comm communicator           communicator in which these processes reside
  ** )
  */
  /* size_t obstacles_size = sizeof(*obstacles_ptr) / sizeof((*obstacles_ptr)[0]);
  ** printf("obstacles_size: %lu\n", obstacles_size);
  ** MPI_Scatter(&obstacles_ptr, obstacles_size/size, MPI_FLOAT, &obstacles_ptr, obstacles_size/size, MPI_FLOAT, MASTER, MPI_COMM_WORLD);
  */
  /* MPI_Scatter(&obstacles_ptr, obstacles_size/size, MPI_FLOAT, &total_obstacles_ptr, obstacles_size/size, MPI_FLOAT, MASTER, MPI_COMM_WORLD); */


  #ifdef DEBUG_init_checkpoints
  printf("Rank %d Checkpoint20\n", rank);
  #endif

  return EXIT_SUCCESS;
}

int timestep(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  accelerate_flow(params, cells, obstacles);
  propagate(params, cells, tmp_cells);
  rebound(params, cells, tmp_cells, obstacles);
  collision(params, cells, tmp_cells, obstacles);
  return EXIT_SUCCESS;
}

int accelerate_flow(const t_param params, t_speed* cells, int* obstacles)
{
  /* compute weighting factors */
  float w1 = params.density * params.accel / 9.f;
  float w2 = params.density * params.accel / 36.f;

  /* modify the 2nd row of the grid */
  int jj = params.ny - 2;

  for (int ii = 0; ii < params.nx; ii++)
  {
    /* if the cell is not occupied and
    ** we don't send a negative density */
    if (!obstacles[ii + jj*params.nx]
        && (cells[ii + jj*params.nx].speeds[3] - w1) > 0.f
        && (cells[ii + jj*params.nx].speeds[6] - w2) > 0.f
        && (cells[ii + jj*params.nx].speeds[7] - w2) > 0.f)
    {
      /* increase 'east-side' densities */
      cells[ii + jj*params.nx].speeds[1] += w1;
      cells[ii + jj*params.nx].speeds[5] += w2;
      cells[ii + jj*params.nx].speeds[8] += w2;
      /* decrease 'west-side' densities */
      cells[ii + jj*params.nx].speeds[3] -= w1;
      cells[ii + jj*params.nx].speeds[6] -= w2;
      cells[ii + jj*params.nx].speeds[7] -= w2;
    }
  }

  return EXIT_SUCCESS;
}

int propagate(const t_param params, t_speed* cells, t_speed* tmp_cells)
{
  /* loop over _all_ cells */
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* determine indices of axis-direction neighbours
      ** respecting periodic boundary conditions (wrap around) */
      int y_n = (jj + 1) % params.ny;
      int x_e = (ii + 1) % params.nx;
      int y_s = (jj == 0) ? (jj + params.ny - 1) : (jj - 1);
      int x_w = (ii == 0) ? (ii + params.nx - 1) : (ii - 1);
      /* propagate densities from neighbouring cells, following
      ** appropriate directions of travel and writing into
      ** scratch space grid */
      tmp_cells[ii + jj*params.nx].speeds[0] = cells[ii + jj*params.nx].speeds[0]; /* central cell, no movement */
      tmp_cells[ii + jj*params.nx].speeds[1] = cells[x_w + jj*params.nx].speeds[1]; /* east */
      tmp_cells[ii + jj*params.nx].speeds[2] = cells[ii + y_s*params.nx].speeds[2]; /* north */
      tmp_cells[ii + jj*params.nx].speeds[3] = cells[x_e + jj*params.nx].speeds[3]; /* west */
      tmp_cells[ii + jj*params.nx].speeds[4] = cells[ii + y_n*params.nx].speeds[4]; /* south */
      tmp_cells[ii + jj*params.nx].speeds[5] = cells[x_w + y_s*params.nx].speeds[5]; /* north-east */
      tmp_cells[ii + jj*params.nx].speeds[6] = cells[x_e + y_s*params.nx].speeds[6]; /* north-west */
      tmp_cells[ii + jj*params.nx].speeds[7] = cells[x_e + y_n*params.nx].speeds[7]; /* south-west */
      tmp_cells[ii + jj*params.nx].speeds[8] = cells[x_w + y_n*params.nx].speeds[8]; /* south-east */
    }
  }

  return EXIT_SUCCESS;
}

int rebound(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  /* loop over the cells in the grid */
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* if the cell contains an obstacle */
      if (obstacles[jj*params.nx + ii])
      {
        /* called after propagate, so taking values from scratch space
        ** mirroring, and writing into main grid */
        cells[ii + jj*params.nx].speeds[1] = tmp_cells[ii + jj*params.nx].speeds[3];
        cells[ii + jj*params.nx].speeds[2] = tmp_cells[ii + jj*params.nx].speeds[4];
        cells[ii + jj*params.nx].speeds[3] = tmp_cells[ii + jj*params.nx].speeds[1];
        cells[ii + jj*params.nx].speeds[4] = tmp_cells[ii + jj*params.nx].speeds[2];
        cells[ii + jj*params.nx].speeds[5] = tmp_cells[ii + jj*params.nx].speeds[7];
        cells[ii + jj*params.nx].speeds[6] = tmp_cells[ii + jj*params.nx].speeds[8];
        cells[ii + jj*params.nx].speeds[7] = tmp_cells[ii + jj*params.nx].speeds[5];
        cells[ii + jj*params.nx].speeds[8] = tmp_cells[ii + jj*params.nx].speeds[6];
      }
    }
  }

  return EXIT_SUCCESS;
}

int collision(const t_param params, t_speed* cells, t_speed* tmp_cells, int* obstacles)
{
  const float c_sq = 1.f / 3.f; /* square of speed of sound */
  const float w0 = 4.f / 9.f;  /* weighting factor */
  const float w1 = 1.f / 9.f;  /* weighting factor */
  const float w2 = 1.f / 36.f; /* weighting factor */

  /* loop over the cells in the grid
  ** NB the collision step is called after
  ** the propagate step and so values of interest
  ** are in the scratch-space grid */
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* don't consider occupied cells */
      if (!obstacles[ii + jj*params.nx])
      {
        /* compute local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += tmp_cells[ii + jj*params.nx].speeds[kk];
        }

        /* compute x velocity component */
        float u_x = (tmp_cells[ii + jj*params.nx].speeds[1]
                      + tmp_cells[ii + jj*params.nx].speeds[5]
                      + tmp_cells[ii + jj*params.nx].speeds[8]
                      - (tmp_cells[ii + jj*params.nx].speeds[3]
                         + tmp_cells[ii + jj*params.nx].speeds[6]
                         + tmp_cells[ii + jj*params.nx].speeds[7]))
                     / local_density;
        /* compute y velocity component */
        float u_y = (tmp_cells[ii + jj*params.nx].speeds[2]
                      + tmp_cells[ii + jj*params.nx].speeds[5]
                      + tmp_cells[ii + jj*params.nx].speeds[6]
                      - (tmp_cells[ii + jj*params.nx].speeds[4]
                         + tmp_cells[ii + jj*params.nx].speeds[7]
                         + tmp_cells[ii + jj*params.nx].speeds[8]))
                     / local_density;

        /* velocity squared */
        float u_sq = u_x * u_x + u_y * u_y;

        /* directional velocity components */
        float u[NSPEEDS];
        u[1] =   u_x;        /* east */
        u[2] =         u_y;  /* north */
        u[3] = - u_x;        /* west */
        u[4] =       - u_y;  /* south */
        u[5] =   u_x + u_y;  /* north-east */
        u[6] = - u_x + u_y;  /* north-west */
        u[7] = - u_x - u_y;  /* south-west */
        u[8] =   u_x - u_y;  /* south-east */

        /* equilibrium densities */
        float d_equ[NSPEEDS];
        /* zero velocity density: weight w0 */
        d_equ[0] = w0 * local_density
                   * (1.f - u_sq / (2.f * c_sq));
        /* axis speeds: weight w1 */
        d_equ[1] = w1 * local_density * (1.f + u[1] / c_sq
                                         + (u[1] * u[1]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[2] = w1 * local_density * (1.f + u[2] / c_sq
                                         + (u[2] * u[2]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[3] = w1 * local_density * (1.f + u[3] / c_sq
                                         + (u[3] * u[3]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[4] = w1 * local_density * (1.f + u[4] / c_sq
                                         + (u[4] * u[4]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        /* diagonal speeds: weight w2 */
        d_equ[5] = w2 * local_density * (1.f + u[5] / c_sq
                                         + (u[5] * u[5]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[6] = w2 * local_density * (1.f + u[6] / c_sq
                                         + (u[6] * u[6]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[7] = w2 * local_density * (1.f + u[7] / c_sq
                                         + (u[7] * u[7]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));
        d_equ[8] = w2 * local_density * (1.f + u[8] / c_sq
                                         + (u[8] * u[8]) / (2.f * c_sq * c_sq)
                                         - u_sq / (2.f * c_sq));

        /* relaxation step */
        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          cells[ii + jj*params.nx].speeds[kk] = tmp_cells[ii + jj*params.nx].speeds[kk]
                                                  + params.omega
                                                  * (d_equ[kk] - tmp_cells[ii + jj*params.nx].speeds[kk]);
        }
      }
    }
  }

  return EXIT_SUCCESS;
}

float av_velocity(const t_param params, t_speed* cells, int* obstacles)
{
  int    tot_cells = 0;  /* no. of cells used in calculation */
  float tot_u;          /* accumulated magnitudes of velocity for each cell */

  /* initialise */
  tot_u = 0.f;

  /* loop over all non-blocked cells */
  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* ignore occupied cells */
      if (!obstacles[ii + jj*params.nx])
      {
        /* local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* x-component of velocity */
        float u_x = (cells[ii + jj*params.nx].speeds[1]
                      + cells[ii + jj*params.nx].speeds[5]
                      + cells[ii + jj*params.nx].speeds[8]
                      - (cells[ii + jj*params.nx].speeds[3]
                         + cells[ii + jj*params.nx].speeds[6]
                         + cells[ii + jj*params.nx].speeds[7]))
                     / local_density;
        /* compute y velocity component */
        float u_y = (cells[ii + jj*params.nx].speeds[2]
                      + cells[ii + jj*params.nx].speeds[5]
                      + cells[ii + jj*params.nx].speeds[6]
                      - (cells[ii + jj*params.nx].speeds[4]
                         + cells[ii + jj*params.nx].speeds[7]
                         + cells[ii + jj*params.nx].speeds[8]))
                     / local_density;
        /* accumulate the norm of x- and y- velocity components */
        tot_u += sqrtf((u_x * u_x) + (u_y * u_y));
        /* increase counter of inspected cells */
        ++tot_cells;
      }
    }
  }

  return tot_u / (float)tot_cells;
}

float total_density(const t_param params, t_speed* cells)
{
  float total = 0.f;  /* accumulator */

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      for (int kk = 0; kk < NSPEEDS; kk++)
      {
        total += cells[ii + jj*params.nx].speeds[kk];
      }
    }
  }

  return total;
}

float calc_reynolds(const t_param params, t_speed* cells, int* obstacles)
{
  const float viscosity = 1.f / 6.f * (2.f / params.omega - 1.f);

  return av_velocity(params, cells, obstacles) * params.reynolds_dim / viscosity;
}

int write_values(const t_param params, t_speed* cells, int* obstacles, float* av_vels)
{
  FILE* fp;                     /* file pointer */
  const float c_sq = 1.f / 3.f; /* sq. of speed of sound */
  float local_density;         /* per grid cell sum of densities */
  float pressure;              /* fluid pressure in grid cell */
  float u_x;                   /* x-component of velocity in grid cell */
  float u_y;                   /* y-component of velocity in grid cell */
  float u;                     /* norm--root of summed squares--of u_x and u_y */

  fp = fopen(FINALSTATEFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int jj = 0; jj < params.ny; jj++)
  {
    for (int ii = 0; ii < params.nx; ii++)
    {
      /* an occupied cell */
      if (obstacles[ii + jj*params.nx])
      {
        u_x = u_y = u = 0.f;
        pressure = params.density * c_sq;
      }
      /* no obstacle */
      else
      {
        local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += cells[ii + jj*params.nx].speeds[kk];
        }

        /* compute x velocity component */
        u_x = (cells[ii + jj*params.nx].speeds[1]
               + cells[ii + jj*params.nx].speeds[5]
               + cells[ii + jj*params.nx].speeds[8]
               - (cells[ii + jj*params.nx].speeds[3]
                  + cells[ii + jj*params.nx].speeds[6]
                  + cells[ii + jj*params.nx].speeds[7]))
              / local_density;
        /* compute y velocity component */
        u_y = (cells[ii + jj*params.nx].speeds[2]
               + cells[ii + jj*params.nx].speeds[5]
               + cells[ii + jj*params.nx].speeds[6]
               - (cells[ii + jj*params.nx].speeds[4]
                  + cells[ii + jj*params.nx].speeds[7]
                  + cells[ii + jj*params.nx].speeds[8]))
              / local_density;
        /* compute norm of velocity */
        u = sqrtf((u_x * u_x) + (u_y * u_y));
        /* compute pressure */
        pressure = local_density * c_sq;
      }

      /* write to file */
      fprintf(fp, "%d %d %.12E %.12E %.12E %.12E %d\n", ii, jj, u_x, u_y, u, pressure, obstacles[ii * params.nx + jj]);
    }
  }

  fclose(fp);

  fp = fopen(AVVELSFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.maxIters; ii++)
  {
    fprintf(fp, "%d:\t%.12E\n", ii, av_vels[ii]);
  }

  fclose(fp);

  return EXIT_SUCCESS;
}

int finalise(const t_param* params, t_speed** cells_ptr, t_speed** tmp_cells_ptr,
             int** obstacles_ptr, float** av_vels_ptr)
{
  /*
  ** free up allocated memory
  */
  free(*cells_ptr);
  *cells_ptr = NULL;

  free(*tmp_cells_ptr);
  *tmp_cells_ptr = NULL;

  free(*obstacles_ptr);
  *obstacles_ptr = NULL;

  free(*av_vels_ptr);
  *av_vels_ptr = NULL;

  return EXIT_SUCCESS;
}

void die(const char* message, const int line, const char* file)
{
  fprintf(stderr, "Error at line %d of file %s:\n", line, file);
  fprintf(stderr, "%s\n", message);
  fflush(stderr);
  exit(EXIT_FAILURE);
}

void usage(const char* exe)
{
  fprintf(stderr, "Usage: %s <paramfile> <obstaclefile>\n", exe);
  exit(EXIT_FAILURE);
}
