// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ivan Sham,
// Andrew Turner, Ali Bakhoda, The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "gpgpusim_entrypoint.h"
#include <stdio.h>

#include "option_parser.h"
#include "cuda-sim/cuda-sim.h"
#include "cuda-sim/ptx_ir.h"
#include "cuda-sim/ptx_parser.h"
#include "gpgpu-sim/gpu-sim.h"
#include "gpgpu-sim/icnt_wrapper.h"
#include "stream_manager.h"
#include "../libcuda/gpgpu_context.h"

#define MAX(a,b) (((a)>(b))?(a):(b))

static int sg_argc = 3;
static const char *sg_argv[] = {"", "-config","gpgpusim.config"};


GPGPUsim_ctx* the_gpgpusim =  NULL;

GPGPUsim_ctx* GPGPUsim_ctx_ptr(){
	if(the_gpgpusim == NULL)
		the_gpgpusim = GPGPU_Context()->the_gpgpusim;

	return the_gpgpusim;
}

class gpgpu_sim* g_the_gpu() {
	return GPGPUsim_ctx_ptr()->g_the_gpu;
}

class stream_manager* g_stream_manager()  {
	return GPGPUsim_ctx_ptr()->g_stream_manager;
}

static void print_simulation_time();

void *gpgpu_sim_thread_sequential(void*)
{
   // at most one kernel running at a time
   bool done;
   do {
      sem_wait(&(GPGPUsim_ctx_ptr()->g_sim_signal_start));
      done = true;
      if( GPGPUsim_ctx_ptr()->g_the_gpu->get_more_cta_left() ) {
          done = false;
          GPGPUsim_ctx_ptr()->g_the_gpu->init();
          while( GPGPUsim_ctx_ptr()->g_the_gpu->active() ) {
        	  GPGPUsim_ctx_ptr()->g_the_gpu->cycle();
        	  GPGPUsim_ctx_ptr()->g_the_gpu->deadlock_check();
          }
          GPGPUsim_ctx_ptr()->g_the_gpu->print_stats();
          GPGPUsim_ctx_ptr()->g_the_gpu->update_stats();
          print_simulation_time();
      }
      sem_post(&(GPGPUsim_ctx_ptr()->g_sim_signal_finish));
   } while(!done);
   sem_post(&(GPGPUsim_ctx_ptr()->g_sim_signal_exit));
   return NULL;
}



static void termination_callback()
{
    printf("GPGPU-Sim: *** exit detected ***\n");
    fflush(stdout);
}

void *gpgpu_sim_thread_concurrent(void*)
{
    atexit(termination_callback);
    // concurrent kernel execution simulation thread
    do {
        printf("GPGPU-Sim: *** simulation thread starting and spinning waiting for work ***\n");
        if(g_debug_execution >= 3) {
           printf("GPGPU-Sim: *** simulation thread starting and spinning waiting for work ***\n");
           fflush(stdout);
        }
        while( GPGPUsim_ctx_ptr()->g_stream_manager->empty_protected() && !GPGPUsim_ctx_ptr()->g_sim_done )
            ;
        printf("GPGPU-Sim: ** START simulation thread (detected work) **\n");
        if(g_debug_execution >= 3) {
           printf("GPGPU-Sim: ** START simulation thread (detected work) **\n");
           GPGPUsim_ctx_ptr()->g_stream_manager->print(stdout);
           fflush(stdout);
        }
        pthread_mutex_lock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
        GPGPUsim_ctx_ptr()->g_sim_active = true;
        pthread_mutex_unlock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
        bool active = false;
        bool sim_cycles = false;
        GPGPUsim_ctx_ptr()->g_the_gpu->init();
        do {
            // check if a kernel has completed
            // launch operation on device if one is pending and can be run

            // Need to break this loop when a kernel completes. This was a
            // source of non-deterministic behaviour in GPGPU-Sim (bug 147).
            // If another stream operation is available, g_the_gpu remains active,
            // causing this loop to not break. If the next operation happens to be
            // another kernel, the gpu is not re-initialized and the inter-kernel
            // behaviour may be incorrect. Check that a kernel has finished and
            // no other kernel is currently running.
            if(GPGPUsim_ctx_ptr()->g_stream_manager->operation(&sim_cycles) && !GPGPUsim_ctx_ptr()->g_the_gpu->active())
                break;

            //functional simulation
            if( GPGPUsim_ctx_ptr()->g_the_gpu->is_functional_sim()) {
                kernel_info_t * kernel = GPGPUsim_ctx_ptr()->g_the_gpu->get_functional_kernel();
                assert(kernel);
                GPGPUsim_ctx_ptr()->gpgpu_ctx->func_sim->gpgpu_cuda_ptx_sim_main_func(*kernel);
                GPGPUsim_ctx_ptr()->g_the_gpu->finish_functional_sim(kernel);
            }

            //performance simulation
            if( GPGPUsim_ctx_ptr()->g_the_gpu->active() ) {
            	GPGPUsim_ctx_ptr()->g_the_gpu->cycle();
            	sim_cycles = true;
            	GPGPUsim_ctx_ptr()->g_the_gpu->deadlock_check();
            }else {
                if(GPGPUsim_ctx_ptr()->g_the_gpu->cycle_insn_cta_max_hit()){
                	GPGPUsim_ctx_ptr()->g_stream_manager->stop_all_running_kernels();
                	GPGPUsim_ctx_ptr()->g_sim_done = true;
                	GPGPUsim_ctx_ptr()->break_limit = true;
                }
            }

            active=GPGPUsim_ctx_ptr()->g_the_gpu->active() || !(GPGPUsim_ctx_ptr()->g_stream_manager->empty_protected());

        } while( active && !GPGPUsim_ctx_ptr()->g_sim_done);
        if(g_debug_execution >= 3) {
           printf("GPGPU-Sim: ** STOP simulation thread (no work) **\n");
           fflush(stdout);
        }
        if(sim_cycles) {
        	GPGPUsim_ctx_ptr()->g_the_gpu->print_stats();
        	GPGPUsim_ctx_ptr()->g_the_gpu->update_stats();
            print_simulation_time();
        }
        pthread_mutex_lock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
        GPGPUsim_ctx_ptr()->g_sim_active = false;
        pthread_mutex_unlock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
    } while( !GPGPUsim_ctx_ptr()->g_sim_done );

    printf("GPGPU-Sim: *** simulation thread exiting ***\n");
    fflush(stdout);

    if(GPGPUsim_ctx_ptr()->break_limit) {
    	printf("GPGPU-Sim: ** break due to reaching the maximum cycles (or instructions) **\n");
    	exit(1);
    }

    sem_post(&(GPGPUsim_ctx_ptr()->g_sim_signal_exit));
    return NULL;
}

bool sst_sim_cycles = false;

bool SST_Cycle()
{
	if( g_stream_manager()->empty_protected() && !GPGPUsim_ctx_ptr()->g_sim_done && !g_the_gpu()->active()) {
		GPGPUsim_ctx_ptr()->g_sim_active = false;
		//printf("stream is empty %d \n",  g_stream_manager->empty());
		return false;
	}

	if(g_stream_manager()->operation(&sst_sim_cycles) && !g_the_gpu()->active()) {
		if(sst_sim_cycles) {
			g_the_gpu()->print_stats();
			g_the_gpu()->update_stats();
			print_simulation_time();
			sst_sim_cycles = false;
		}
		return false;
	}

	//printf("GPGPU-Sim: Give GPU Cycle\n");
	GPGPUsim_ctx_ptr()->g_sim_active = true;

	//functional simulation
	if( g_the_gpu()->is_functional_sim()) {
		kernel_info_t * kernel = g_the_gpu()->get_functional_kernel();
		assert(kernel);
		GPGPUsim_ctx_ptr()->gpgpu_ctx->func_sim->gpgpu_cuda_ptx_sim_main_func(*kernel);
		g_the_gpu()->finish_functional_sim(kernel);
	}

	//performance simulation
	if( g_the_gpu()->active() ) {
		g_the_gpu()->SST_cycle();
		sst_sim_cycles = true;
		g_the_gpu()->deadlock_check();
	}else {
		if(g_the_gpu()->cycle_insn_cta_max_hit()){
			g_stream_manager()->stop_all_running_kernels();
			GPGPUsim_ctx_ptr()->g_sim_done = true;
			GPGPUsim_ctx_ptr()->g_sim_active = false;
			GPGPUsim_ctx_ptr()->break_limit = true;
		}
	}

    if(GPGPUsim_ctx_ptr()->break_limit) {
    	printf("GPGPU-Sim: ** break due to reaching the maximum cycles (or instructions) **\n");
    	return true;
    }

    return false;

}

void synchronize()
{
    printf("GPGPU-Sim: synchronize waiting for inactive GPU simulation\n");
    GPGPUsim_ctx_ptr()->g_stream_manager->print(stdout);
    fflush(stdout);
//    sem_wait(&g_sim_signal_finish);
    if( g_the_gpu()->is_SST_mode()) {
    	printf("synchronize is not implemented yet in SST mode!\n");
    	assert(0);
    }
    else {
    bool done = false;
    do {
        pthread_mutex_lock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
        done = ( GPGPUsim_ctx_ptr()->g_stream_manager->empty() && !GPGPUsim_ctx_ptr()->g_sim_active ) || GPGPUsim_ctx_ptr()->g_sim_done;
        pthread_mutex_unlock(&(GPGPUsim_ctx_ptr()->g_sim_lock));
    } while (!done);
    printf("GPGPU-Sim: detected inactive GPU simulation thread\n");
    fflush(stdout);
//    sem_post(&g_sim_signal_start);
    }
}

void exit_simulation()
{
    GPGPUsim_ctx_ptr()->g_sim_done=true;
    printf("GPGPU-Sim: exit_simulation called\n");
    fflush(stdout);
    if( !g_the_gpu()->is_SST_mode()) {
    	//No need to wait for the exit of the thread in SST mode
    	sem_wait(&(GPGPUsim_ctx_ptr()->g_sim_signal_exit));
        printf("GPGPU-Sim: simulation thread signaled exit\n");
        fflush(stdout);
    }
}

gpgpu_sim *gpgpu_context::gpgpu_ptx_sim_init_perf()
{
   srand(1);
   print_splash();
   func_sim->read_sim_environment_variables();
   ptx_parser->read_parser_environment_variables();
   option_parser_t opp = option_parser_create();

   ptx_reg_options(opp);
   func_sim->ptx_opcocde_latency_options(opp);

   icnt_reg_options(opp);
   GPGPUsim_ctx_ptr()->g_the_gpu_config = new gpgpu_sim_config(this);
   GPGPUsim_ctx_ptr()->g_the_gpu_config->reg_options(opp); // register GPU microrachitecture options

   option_parser_cmdline(opp, sg_argc, sg_argv); // parse configuration options
   fprintf(stdout, "GPGPU-Sim: Configuration options:\n\n");
   option_parser_print(opp, stdout);
   // Set the Numeric locale to a standard locale where a decimal point is a "dot" not a "comma"
   // so it does the parsing correctly independent of the system environment variables
   assert(setlocale(LC_NUMERIC,"C"));
   GPGPUsim_ctx_ptr()->g_the_gpu_config->init();

   GPGPUsim_ctx_ptr()->g_the_gpu = new gpgpu_sim(*(GPGPUsim_ctx_ptr()->g_the_gpu_config), this);
   GPGPUsim_ctx_ptr()->g_stream_manager = new stream_manager((GPGPUsim_ctx_ptr()->g_the_gpu), func_sim->g_cuda_launch_blocking);

   GPGPUsim_ctx_ptr()->g_simulation_starttime = time((time_t *)NULL);

   sem_init(&(GPGPUsim_ctx_ptr()->g_sim_signal_start),0,0);
   sem_init(&(GPGPUsim_ctx_ptr()->g_sim_signal_finish),0,0);
   sem_init(&(GPGPUsim_ctx_ptr()->g_sim_signal_exit),0,0);

   return GPGPUsim_ctx_ptr()->g_the_gpu;
}

void start_sim_thread(int api)
{
    if( GPGPUsim_ctx_ptr()->g_sim_done ) {
        GPGPUsim_ctx_ptr()->g_sim_done = false;
        if( !g_the_gpu()->is_SST_mode()) {
        	//Do not create the concurrent thread in the SST mode
			if( api == 1 ) {
			   pthread_create(&(GPGPUsim_ctx_ptr()->g_simulation_thread),NULL,gpgpu_sim_thread_concurrent,NULL);
			} else {
			   pthread_create(&(GPGPUsim_ctx_ptr()->g_simulation_thread),NULL,gpgpu_sim_thread_sequential,NULL);
			}
        } else {
        	//do the init for SST mode
        	   g_the_gpu()->init();
        }
    }
}

void print_simulation_time()
{
   time_t current_time, difference, d, h, m, s;
   current_time = time((time_t *)NULL);
   difference = MAX(current_time - GPGPUsim_ctx_ptr()->g_simulation_starttime, 1);

   d = difference/(3600*24);
   h = difference/3600 - 24*d;
   m = difference/60 - 60*(h + 24*d);
   s = difference - 60*(m + 60*(h + 24*d));

   fflush(stderr);
   printf("\n\ngpgpu_simulation_time = %u days, %u hrs, %u min, %u sec (%u sec)\n",
          (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)s, (unsigned)difference );
   printf("gpgpu_simulation_rate = %u (inst/sec)\n", (unsigned)(GPGPUsim_ctx_ptr()->g_the_gpu->gpu_tot_sim_insn / difference) );
   printf("gpgpu_simulation_rate = %u (cycle/sec)\n", (unsigned)(GPGPUsim_ctx_ptr()->g_the_gpu->gpu_tot_sim_cycle / difference) );
   fflush(stdout);
}

int gpgpu_opencl_ptx_sim_main_perf( kernel_info_t *grid )
{
   GPGPUsim_ctx_ptr()->g_the_gpu->launch(grid);
   sem_post(&(GPGPUsim_ctx_ptr()->g_sim_signal_start));
   sem_wait(&(GPGPUsim_ctx_ptr()->g_sim_signal_finish));
   return 0;
}

//! Functional simulation of OpenCL
/*!
 * This function call the CUDA PTX functional simulator
 */
int cuda_sim::gpgpu_opencl_ptx_sim_main_func( kernel_info_t *grid )
{
    //calling the CUDA PTX simulator, sending the kernel by reference and a flag set to true,
    //the flag used by the function to distinguish OpenCL calls from the CUDA simulation calls which
    //it is needed by the called function to not register the exit the exit of OpenCL kernel as it doesn't register entering in the first place as the CUDA kernels does
   gpgpu_cuda_ptx_sim_main_func( *grid, true );
   return 0;
}
