#include <sst_config.h>
#include <string>
#include <iostream>

#include "Gpgpusim.h"

using namespace SST;
using namespace SST::Interfaces;
using namespace SST::GpgpusimComponent;


//This is a global pointer to gpgpu-sim component  in order to be accessed from gpgpu-sim
SST::GpgpusimComponent::Gpgpusim* my_gpu_component = NULL;

Gpgpusim::Gpgpusim(SST::ComponentId_t id, SST::Params& params): Component(id) {

    //registerAsPrimaryComponent();
    //primaryComponentDoNotEndSim();

    cpu_core_count = (uint32_t) params.find<uint32_t>("cpu_cores", 1);
    gpu_core_count = (uint32_t) params.find<uint32_t>("gpu_cores", 1);
    latency = (uint32_t) params.find<uint32_t>("latency", 1);
    maxPendingTransCore = (uint32_t) params.find<uint32_t>("maxtranscore", 16);
    maxPendingCacheTrans = (uint32_t) params.find<uint32_t>("maxcachetrans", 512);

    int verbosity = params.find<int>("verbose", 0);
    output = new SST::Output("GpgpusimComponent[@f:@l:@p] ", verbosity, 0, SST::Output::STDOUT);

    //Ensure that GPGP-sim has the same as SST gpu cores
    SST_gpgpusim_numcores_equal_check(gpu_core_count);

    pending_transactions_count = 0;
    remainingTransfer = 0;
    totalTransfer = 0;
    ackTransfer = 0;
    transferNumber = 0;

    // Link names
    char* link_buffer = (char*) malloc(sizeof(char) * 256);
    char* link_buffer_mem = (char*) malloc(sizeof(char) * 256);
    char* link_cache_buffer = (char*) malloc(sizeof(char) * 256);

    // Buffer to keep track of pending transactions
    pendingTransactions = new std::unordered_map<SimpleMem::Request::id_t, SimpleMem::Request*>();
    gpuCachePendingTransactions = new std::unordered_map<SimpleMem::Request::id_t, cache_req_params>();

    // CPU link allocation
    gpu_to_cpu_cache_links = (SimpleMem**) malloc( sizeof(SimpleMem*) * cpu_core_count );
    gpu_to_core_links = (Link**) malloc( sizeof(Link*) * cpu_core_count );

    for(uint32_t i = 0; i < cpu_core_count; i++) {
        // Create a unique name for all links
        sprintf(link_buffer, "requestLink%" PRIu32, i);
        sprintf(link_buffer_mem, "requestMemLink%" PRIu32, i);

        // Create a link back to ArielCore
        gpu_to_core_links[i] = configureLink(link_buffer, "1ns", new Event::Handler<Gpgpusim>(this, &Gpgpusim::cpuHandler));

        // Create and initialize CPU memHierarchy links (SimpleMem)
        gpu_to_cpu_cache_links[i] = dynamic_cast<SimpleMem*>(loadSubComponent("memHierarchy.memInterface", this, params));
        gpu_to_cpu_cache_links[i]->initialize(link_buffer_mem, new SimpleMem::Handler<Gpgpusim>(this, &Gpgpusim::memoryHandler));
        gpu_to_cpu_cache_links[i]->init(0);

    }

    gpu_to_cache_links = (SimpleMem**) malloc( sizeof(SimpleMem*) * gpu_core_count );
    numPendingCacheTransPerCore = new uint32_t[gpu_core_count];

    //// GPU's Cache link allocation
    //// GPU memory Hirerchy
    for(uint32_t i = 0; i < gpu_core_count; i++) {

    	 // Create a unique name for all links
    	 sprintf(link_cache_buffer, "requestGPUCacheLink%" PRIu32, i);

        // Create and initialize GPU memHierarchy links (SimpleMem)
        gpu_to_cache_links[i] = dynamic_cast<SimpleMem*>(loadSubComponent("memHierarchy.memInterface", this, params));
        gpu_to_cache_links[i]->initialize(link_cache_buffer, new SimpleMem::Handler<Gpgpusim>(this, &Gpgpusim::gpuCacheHandler));
        gpu_to_cache_links[i]->init(0);

        numPendingCacheTransPerCore[i] = 0;

    }

    std::string gpu_clock = params.find<std::string>("clock", "1GHz");
    registerClock( gpu_clock, new Clock::Handler<Gpgpusim>(this, &Gpgpusim::tick ) );

    my_gpu_component = this;
}

Gpgpusim::Gpgpusim() : Component(-1)
{
    // for serialization only
}

bool Gpgpusim::tick(SST::Cycle_t x)
{
	 bool done = SST_gpu_core_cycle();
	 return done;
}

void Gpgpusim::handleCPUReadRequest(size_t txSize, uint64_t pAddr){
    uint64_t addrOffset = physicalAddresses[0] % 64;
    SimpleMem::Request *req;

    if(txSize + addrOffset <= 64) {
        physicalAddresses[0] += txSize;
    }else {
        uint64_t leftAddr = currentAddress;
        uint64_t leftSize = 64 - addrOffset;
        uint64_t rightAddr = currentAddress + 64 - addrOffset;
        uint64_t physLeftAddr = physicalAddresses[0];
        uint64_t physRightAddr = physLeftAddr += leftSize;
        req = new SimpleMem::Request(SimpleMem::Request::Read, leftAddr, txSize);

    }
}

void Gpgpusim::handleCPUWriteRequest(size_t txSize, uint64_t pAddr){

}

void Gpgpusim::cpuHandler( SST::Event* e ){
    GpgpusimEvent * temp_ptr =  dynamic_cast<GpgpusimComponent::GpgpusimEvent*> (e);
    if(temp_ptr->getType() == EventType::REQUEST){

    output->verbose(CALL_INFO, 4, 0, "GPU received a request event: ");
    GpgpusimEvent * tse = new GpgpusimEvent(EventType::RESPONSE);
    switch(temp_ptr->API){
        case GPU_REG_FAT_BINARY:
             tse->CA.register_fatbin.fat_cubin_handle = __cudaRegisterFatBinarySST(temp_ptr->CA.file_name);
             tse->API = GPU_REG_FAT_BINARY_RET;
             break;
        case GPU_REG_FUNCTION:
             __cudaRegisterFunctionSST(temp_ptr->CA.register_function.fat_cubin_handle, temp_ptr->CA.register_function.host_fun, temp_ptr->CA.register_function.device_fun);
             tse->API = GPU_REG_FUNCTION_RET;
             break;
        case GPU_MEMCPY:
            {
                // Extract fields ahead of time for the sake of clarity
                uint64_t src = temp_ptr->CA.cuda_memcpy.src;
                uint64_t dst = temp_ptr->CA.cuda_memcpy.dst;
                size_t count = temp_ptr->CA.cuda_memcpy.count;
                cudaMemcpyKind kind = temp_ptr->CA.cuda_memcpy.kind;
                physicalAddresses = temp_ptr->payload;

                is_stalled = true;
                size_t current_transfer = 0;
                uint64_t addr_offset;
                tse->API = GPU_MEMCPY_RET;
                memcpyKind = kind;
                totalTransfer = count;
                remainingTransfer = count;
                ackTransfer = 0;
                dataAddress.resize(count);
                transferNumber = 0;
                if(kind == cudaMemcpyHostToDevice){
		    output->verbose(CALL_INFO, 8, 0, "CUDA memcpy H2D from %" PRIu64 " to %" PRIu64 " of size %" PRIu32 "\n", src, dst, count);
                    baseAddress = dst;
                    currentAddress = dst;
                    while((pending_transactions_count < maxPendingTransCore) && (remainingTransfer > 0)){
                        current_transfer = (remainingTransfer > 64) ? 64 : remainingTransfer;
                        addr_offset = physicalAddresses[0] % 64;
                        SimpleMem::Request *req;
                        if((addr_offset + current_transfer <= 64)){
                            output->verbose(CALL_INFO, 4, 0, "CUDA GPU non-split read physical address %" PRIu64 " of size %" PRIu32 "\n", physicalAddresses[0], current_transfer);
                            req = new SimpleMem::Request(SimpleMem::Request::Read, physicalAddresses[0], current_transfer);
                            physicalAddresses[0] += current_transfer;
                        } else{
                            uint64_t leftAddr = currentAddress;
                            uint64_t leftSize = 64 - addr_offset;
                            uint64_t rightAddr = currentAddress + 64 - addr_offset;
                            uint64_t physLeftAddr = physicalAddresses[0];
                            uint64_t physRightAddr = physLeftAddr += leftSize;
                            current_transfer = leftSize;
                            output->verbose(CALL_INFO, 4, 0, "CUDA GPU split read physical address %" PRIu64 " of size %" PRIu32 "\n", physLeftAddr, current_transfer);
                            // Send one request, second request right address will be done next
                            physicalAddresses[0] = physRightAddr;
                            req = new SimpleMem::Request(SimpleMem::Request::Read, physLeftAddr, current_transfer);
                        }

                        req->setVirtualAddress(currentAddress);
                        remainingTransfer -= current_transfer;
                        currentAddress += current_transfer;
                        transferNumber += 1;

                        output->verbose(CALL_INFO, 4, 0, "CUDA GPU remaining data transfer bytes %" PRIu32 "\n", remainingTransfer);

                        pending_transactions_count++;
                        pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );
                        gpu_to_cpu_cache_links[0]->sendRequest(req);
                    }
                } else if(kind == cudaMemcpyDeviceToHost){
                    output->verbose(CALL_INFO, 4, 0, "Within Device To Host\n");
                    baseAddress = src;
                    currentAddress = src;
                    cudaMemcpy(&dataAddress[0], (void*)baseAddress, count, kind);
                    //Wait untill it is done

                } if (kind == cudaMemcpyDeviceToDevice) {
                    is_stalled = false;
                    output->verbose(CALL_INFO, 4, 0, "Within Device To Device\n");
                    output->verbose(CALL_INFO, 4, 0, "CUDA memcpy D2D from %" PRIu64 " to %" PRIu64 " of size %" PRIu32 "\n", src, dst, count);
                    cudaMemcpy((void*)dst, (void*)src, count, kind);
                } else {
                    // Other cudaMemcpy types TODO here
                }
            }
            break;
        case GPU_CONFIG_CALL:
        {
            dim3 gridDim;
            gridDim.x = temp_ptr->CA.cfg_call.gdx;
            gridDim.y = temp_ptr->CA.cfg_call.gdy;
            gridDim.z = temp_ptr->CA.cfg_call.gdz;
            dim3 blockDim;
            blockDim.x = temp_ptr->CA.cfg_call.bdx;
            blockDim.y = temp_ptr->CA.cfg_call.bdy;
            blockDim.z = temp_ptr->CA.cfg_call.bdz;
            cudaConfigureCallSST(gridDim, blockDim, temp_ptr->CA.cfg_call.sharedMem, temp_ptr->CA.cfg_call.stream);
            tse->API = GPU_CONFIG_CALL_RET;
        }
            break;
        case GPU_SET_ARG:
        {
             cudaSetupArgumentSST(temp_ptr->CA.set_arg.address, temp_ptr->CA.set_arg.value, temp_ptr->CA.set_arg.size, temp_ptr->CA.set_arg.offset);
             tse->API = GPU_SET_ARG_RET;
        }
            break;
        case GPU_LAUNCH:
            output->verbose(CALL_INFO, 4, 0, "GPU KERNEL LAUNCHING\n");
            cudaLaunchSST(temp_ptr->CA.cuda_launch.func);
            tse->API = GPU_LAUNCH_RET;
            break;
        case GPU_FREE:
            cudaFree((void*)temp_ptr->CA.free_address);
            tse->API = GPU_FREE_RET;
            break;
        case GPU_GET_LAST_ERROR:
            tse->API = GPU_GET_LAST_ERROR_RET;
            break;
        case GPU_MALLOC:
            tse->CA.cuda_malloc.ptr_address =  cudaMallocSST(temp_ptr->CA.cuda_malloc.dev_ptr, temp_ptr->CA.cuda_malloc.size);
            tse->API = GPU_MALLOC_RET;
            break;
        case GPU_REG_VAR:
            __cudaRegisterVar((void **)temp_ptr->CA.register_var.fatCubinHandle, (char *)temp_ptr->CA.register_var.hostVar,
                    (char *)temp_ptr->CA.register_var.deviceName, (const char *)temp_ptr->CA.register_var.deviceName,
                    temp_ptr->CA.register_var.ext, temp_ptr->CA.register_var.size,
                    temp_ptr->CA.register_var.constant, temp_ptr->CA.register_var.global);
            tse->API = GPU_REG_VAR_RET;
            break;
        case GPU_MAX_BLOCK:
            cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(
                    &(tse->CA.max_active_block.numBlock),
                    (const char *)(temp_ptr->CA.max_active_block.hostFunc),
                    temp_ptr->CA.max_active_block.blockSize,
                    temp_ptr->CA.max_active_block.dynamicSMemSize,
                    temp_ptr->CA.max_active_block.flags
                    );
            tse->API = GPU_MAX_BLOCK_RET;
            break;
        default:
            //TODO actually fail here
            break;
        }

        if(!is_stalled){
            gpu_to_core_links[0]->send(latency, tse);
            output->verbose(CALL_INFO, 6, 0, "GPU sent a ACK\n");
        }
    }
}

void Gpgpusim::gpuCacheHandler(SimpleMem::Request* event){

    SimpleMem::Request::id_t mev_id = event->id;
	auto find_entry = gpuCachePendingTransactions->find(mev_id);
	if(find_entry != gpuCachePendingTransactions->end()) {
		cache_req_params req_params = find_entry->second;
		gpuCachePendingTransactions->erase(gpuCachePendingTransactions->find(mev_id));
		assert(numPendingCacheTransPerCore[req_params.core_id] > 0);
		numPendingCacheTransPerCore[req_params.core_id]--;
		SST_receive_mem_reply( req_params.core_id,  req_params.mem_fetch_pointer);
		delete event;
	}
	else {
            assert("\n Cannot find the request\n" &&  0);
	}
}

void Gpgpusim::memoryHandler(SimpleMem::Request* event){
    SimpleMem::Request::id_t mev_id = event->id;
    auto find_entry = pendingTransactions->find(mev_id);
    if(find_entry != pendingTransactions->end()) {
        int size = event->data.size();
        int index = event->getVirtualAddress() - baseAddress;
        ackTransfer += size;
        
        output->verbose(CALL_INFO, 4, 0, "CUDA total GPU ACK %" PRIu32 " of total %" PRIu32 "\n", ackTransfer, totalTransfer);
        // Finished receiving data for a CUDA memcpy operation
        if(ackTransfer == totalTransfer){
            GpgpusimEvent * tse = new GpgpusimEvent(EventType::RESPONSE);
            tse->API = GPU_MEMCPY_RET;
            if(memcpyKind == cudaMemcpyHostToDevice){
                output->verbose(CALL_INFO, 4, 0, "Memcpy host to device\n");
                for(int i = 0; i < event->data.size(); i++){
                    dataAddress[index+i] = event->data[i];
                }

                output->verbose(CALL_INFO, 8, 0, "CUDA calling cudaMemcpySST on data: ");
		if( static_cast<uint32_t>(output->getVerboseLevel()) >= 8) {
                    for(int i = 0; i < dataAddress.size(); i++)
                        printf("%d ", dataAddress[i]);
                    printf("\n");
                }

                // Done with memcpy, call GPGPU-Sim
                cudaMemcpySST(baseAddress, event->addr, totalTransfer, memcpyKind, &dataAddress[0]);
                pending_transactions_count--;
                pendingTransactions->erase(pendingTransactions->find(mev_id));
            }else if(memcpyKind == cudaMemcpyDeviceToHost){
                output->verbose(CALL_INFO, 4, 0, "Memcpy device to host\n");
                tse->CA.cuda_memcpy.src = baseAddress;
                tse->CA.cuda_memcpy.count = totalTransfer;
                tse->CA.cuda_memcpy.kind = memcpyKind;
                tse->CA.cuda_memcpy.dst = event->addr;
                // Free the GPU and send an ACK
                pending_transactions_count--;
                pendingTransactions->erase(pendingTransactions->find(mev_id));
                is_stalled = false;
                gpu_to_core_links[0]->send(latency, tse);
            }

            output->verbose(CALL_INFO, 4, 0, "CUDA GPU sent a ACK\n");
        } else {
            if(memcpyKind == cudaMemcpyHostToDevice){
                // Copy data into local vector until we've received it all
                for(int i = 0; i < event->data.size(); i++){
                    dataAddress[index+i] = event->data[i];
                }
            }else if(memcpyKind == cudaMemcpyDeviceToHost){
                // Continue on waiting for more writes to come back
            }
            pending_transactions_count--;
            pendingTransactions->erase(pendingTransactions->find(mev_id));
        }
        // Remove transaction, then check if we need to send anything else
        if(remainingTransfer != 0){
            size_t current_transfer;
            uint64_t addr_offset;
            SimpleMem::Request *req;

            output->verbose(CALL_INFO, 4, 0, "CUDA total GPU remaining transfer is %" PRIu32 " open transactions %" PRIu32 "\n", remainingTransfer, pending_transactions_count);
            if(memcpyKind == cudaMemcpyHostToDevice){
                // Send reads while we still can
                while((pending_transactions_count < maxPendingTransCore) && (remainingTransfer != 0)){
                    current_transfer = (remainingTransfer > 64) ? 64 : remainingTransfer;
                    addr_offset = physicalAddresses[0] % 64;
                    SimpleMem::Request *req;
                    if((addr_offset + current_transfer <= 64)){
                        output->verbose(CALL_INFO, 4, 0, "CUDA GPU non-split read physical address %" PRIu64 " of size %" PRIu32 "\n", physicalAddresses[0], current_transfer);
                        req = new SimpleMem::Request(SimpleMem::Request::Read, physicalAddresses[0], current_transfer);
                        physicalAddresses[0] += current_transfer;
                    } else{
                        uint64_t leftAddr = currentAddress;
                        uint64_t leftSize = 64 - addr_offset;
                        uint64_t rightAddr = currentAddress + 64 - addr_offset;
                        uint64_t physLeftAddr = physicalAddresses[0];
                        uint64_t physRightAddr = physLeftAddr += leftSize;
                        current_transfer = leftSize;

                        output->verbose(CALL_INFO, 4, 0, "CUDA GPU split read physical address %" PRIu64 " of size %" PRIu32 "\n", physLeftAddr, current_transfer);
                        // Send one request, second request right address will be done next
                        physicalAddresses[0] = physRightAddr;
                        req = new SimpleMem::Request(SimpleMem::Request::Read, physLeftAddr, current_transfer);
                    }

                    // Update current transfer status
                    req->setVirtualAddress(currentAddress);
                    remainingTransfer -= current_transfer;
                    currentAddress += current_transfer;
                    output->verbose(CALL_INFO, 4, 0, "CUDA GPU remaining data transfer bytes %" PRIu32 "\n", remainingTransfer);

                    // Send to memHierarchy
                    pending_transactions_count++;
                    pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );
                    gpu_to_cpu_cache_links[0]->sendRequest(req);
                }
            } else if(memcpyKind == cudaMemcpyDeviceToHost){
                // Send as many writes as we can
                while((pending_transactions_count < maxPendingTransCore) && (remainingTransfer > 0)){
                    index = currentAddress - baseAddress;
                    current_transfer = (remainingTransfer > 64) ? 64 : remainingTransfer;
                    addr_offset = physicalAddresses[0] % 64;
                    SimpleMem::Request *req;
                    if((addr_offset + current_transfer <= 64)){
                        req = new SimpleMem::Request(SimpleMem::Request::Write, physicalAddresses[0], current_transfer);
                        physicalAddresses[0] += current_transfer;
                    } else{
                        uint64_t leftAddr = currentAddress;
                        uint64_t leftSize = 64 - addr_offset;
                        uint64_t rightAddr = currentAddress + 64 - addr_offset;
                        uint64_t physLeftAddr = physicalAddresses[0];
                        uint64_t physRightAddr = physLeftAddr += leftSize;
                        current_transfer = leftSize;
                        physicalAddresses[0] = physRightAddr;
                        req = new SimpleMem::Request(SimpleMem::Request::Read, physLeftAddr, current_transfer);
                    }
                    // Update current transfer status
                    remainingTransfer -= current_transfer;
                    currentAddress += current_transfer;
                    transferNumber += 1;
                    // Send to memHierarchy
                    req->setVirtualAddress(currentAddress);
                    req->setPayload(&dataAddress[index], current_transfer);
                    pending_transactions_count++;
                    pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );
                    gpu_to_cpu_cache_links[0]->sendRequest(req);
                }
            }
            // Do nothing, just waiting on more events return from memHierarchy
        }
        // Delete event, send return message if done
        delete event;
    }else {
        // Should fail if not found
    }


}

bool Gpgpusim::is_SST_buffer_full(unsigned core_id)
{
	return (numPendingCacheTransPerCore[core_id] == maxPendingCacheTrans);
}

void Gpgpusim::send_read_request_SST(unsigned core_id, uint64_t address, size_t size, void* mem_req)
{
	assert(numPendingCacheTransPerCore[core_id] < maxPendingCacheTrans);
	SimpleMem::Request* req = new SimpleMem::Request(SimpleMem::Request::Read, address, size);
	req->setVirtualAddress(address);
	cache_req_params crp(core_id, mem_req, req);
	gpuCachePendingTransactions->insert( std::pair<SimpleMem::Request::id_t, cache_req_params>(req->id, crp) );
	numPendingCacheTransPerCore[core_id]++;
	gpu_to_cache_links[core_id]->sendRequest(req);
}

void Gpgpusim::send_write_request_SST(unsigned core_id, uint64_t address, size_t size, void* mem_req)
{
	assert(numPendingCacheTransPerCore[core_id] < maxPendingCacheTrans);
	SimpleMem::Request* req = new SimpleMem::Request(SimpleMem::Request::Write, address, size);
	req->setVirtualAddress(address);
	gpuCachePendingTransactions->insert( std::pair<SimpleMem::Request::id_t, cache_req_params>(req->id, cache_req_params(core_id, mem_req, req)) );
	numPendingCacheTransPerCore[core_id]++;
	gpu_to_cache_links[core_id]->sendRequest(req);
}

void Gpgpusim::SST_callback_memcpy_H2D_done()
{

	GpgpusimEvent * tse = new GpgpusimEvent(EventType::RESPONSE);
	tse->API = GPU_MEMCPY_RET;
	is_stalled = false;
	gpu_to_core_links[0]->send(latency, tse);

}

void Gpgpusim::SST_callback_memcpy_D2H_done()
{
	int index;
	uint64_t addr_offset;
	size_t current_transfer = 0;
	size_t count = totalTransfer;

	output->verbose(CALL_INFO, 4, 0, "CUDA calculated result %" PRIu64 "", baseAddress);
        if( static_cast<uint32_t>(output->getVerboseLevel()) >= 8) {
	    for(int i = 0; i < count; i++)
	    	printf("%d ", dataAddress[i]);
            printf("\n");
        }

	while((pending_transactions_count < maxPendingTransCore) && (remainingTransfer != 0)){
		index = currentAddress - baseAddress;
		current_transfer = (remainingTransfer > 64) ? 64 : remainingTransfer;
		addr_offset = physicalAddresses[0] % 64;
		SimpleMem::Request *req;
		if((addr_offset + current_transfer <= 64)){
			output->verbose(CALL_INFO, 8, 0, "CUDA GPU non-split write physical address %p\n", physicalAddresses[0]);
			req = new SimpleMem::Request(SimpleMem::Request::Write, physicalAddresses[0], current_transfer);
			req->setVirtualAddress(currentAddress);
			req->setPayload(&(dataAddress[index]), current_transfer);
			physicalAddresses[0] += current_transfer;
		} else{
			uint64_t leftAddr = currentAddress;
			uint64_t leftSize = 64 - addr_offset;
			uint64_t rightAddr = currentAddress + 64 - addr_offset;
			uint64_t physLeftAddr = physicalAddresses[0];
			uint64_t physRightAddr = physLeftAddr += leftSize;
			current_transfer = leftSize;
			output->verbose(CALL_INFO, 8, 0, "CUDA GPU split write physical address %p %p\n", physLeftAddr, physRightAddr);
			// Send one request, second request will be done next loop iteration
			physicalAddresses[0] = physRightAddr;
			req = new SimpleMem::Request(SimpleMem::Request::Write, physLeftAddr, current_transfer);
			req->setVirtualAddress(currentAddress);
			req->setPayload(&(dataAddress[index]), current_transfer);
		}

		remainingTransfer -= current_transfer;
		currentAddress += current_transfer;
		transferNumber += 1;
		pending_transactions_count++;
		pendingTransactions->insert( std::pair<SimpleMem::Request::id_t, SimpleMem::Request*>(req->id, req) );
		gpu_to_cpu_cache_links[0]->sendRequest(req);
	}

}


//Global Wrappers
extern bool is_SST_buffer_full(unsigned core_id)
{
	assert(my_gpu_component);
	return my_gpu_component->is_SST_buffer_full(core_id);
}

extern void send_read_request_SST(unsigned core_id, uint64_t address, size_t size, void* mem_req)
{
	assert(my_gpu_component);
	my_gpu_component->send_read_request_SST(core_id, address, size, mem_req);

}

extern void send_write_request_SST(unsigned core_id, uint64_t address, size_t size, void* mem_req)
{
	assert(my_gpu_component);
	my_gpu_component->send_write_request_SST(core_id, address, size, mem_req);
}

extern void SST_callback_memcpy_H2D_done()
{
	assert(my_gpu_component);
	my_gpu_component->SST_callback_memcpy_H2D_done();
}

extern void SST_callback_memcpy_D2H_done()
{
	assert(my_gpu_component);
	my_gpu_component->SST_callback_memcpy_D2H_done();
}
