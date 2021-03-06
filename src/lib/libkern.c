/*
 * libkern.c - Everything that touches the kernel.
 *
 * Copyright (c) 2014 Samuel Groß
 * Copyright (c) 2016 Siguza
 */

#include <stdlib.h>             // free, malloc
#include <string.h>             // memmem

#include <mach/host_priv.h>     // host_get_special_port
#include <mach/kern_return.h>   // KERN_SUCCESS, kern_return_t
#include <mach/mach_init.h>     // mach_host_self, mach_task_self
#include <mach/message.h>       // mach_msg_type_number_t
#include <mach/mach_error.h>    // mach_error_string
#include <mach/mach_traps.h>    // task_for_pid
#include <mach/mach_types.h>    // task_t
#include <mach/port.h>          // MACH_PORT_NULL, MACH_PORT_VALID
#include <mach/vm_prot.h>       // VM_PROT_READ, VM_PROT_WRITE, VM_PROT_EXECUTE
#include <mach/vm_region.h>     // VM_REGION_SUBMAP_INFO_COUNT_64, vm_region_info_t, vm_region_submap_info_data_64_t
#include <mach/vm_map.h>        // vm_read_overwrite, vm_region_recurse_64, vm_write
#include <mach/vm_types.h>      // vm_address_t, vm_size_t
#include <mach-o/loader.h>      // MH_EXECUTE

#include "arch.h"               // IMAGE_OFFSET, MACH_TYPE, MACH_HEADER_MAGIC, mach_hdr_t
#include "debug.h"              // BUGTRACKER_URL, DEBUG
#include "libkern.h"

#define MAX_CHUNK_SIZE 0xFFF

#define VERIFY_PORT(port, ret) \
do \
{ \
    if(MACH_PORT_VALID(port)) \
    { \
        if(ret == KERN_SUCCESS) \
        { \
            DEBUG("Success!"); \
        } \
        else \
        { \
            DEBUG("Got a valid port, but return value is 0x%08x (%s)", ret, mach_error_string(ret)); \
            ret = KERN_SUCCESS; \
        } \
    } \
    else \
    { \
        if(ret == KERN_SUCCESS) \
        { \
            DEBUG("Returned success, but port is invalid (0x%08x)", port); \
            ret = KERN_FAILURE; \
        } \
        else \
        { \
            DEBUG("Failure. Port: 0x%08x, return value: 0x%08x (%s)", port, ret, mach_error_string(ret)); \
        } \
    } \
} while(0)

kern_return_t get_kernel_task(task_t *task)
{
    static task_t kernel_task = MACH_PORT_NULL;
    static char initialized = 0;
    DEBUG("Getting kernel task...");
    if(initialized)
    {
        DEBUG("Already happened, returning cached value.");
    }
    else
    {
        DEBUG("Trying task_for_pid(0)...");
        kernel_task = MACH_PORT_NULL;
        kern_return_t ret = task_for_pid(mach_task_self(), 0, &kernel_task);
        VERIFY_PORT(kernel_task, ret);
        if(ret != KERN_SUCCESS)
        {
            // Try Pangu's special port
            DEBUG("Trying host_get_special_port(4)...");
            kernel_task = MACH_PORT_NULL;
            ret = host_get_special_port(mach_host_self(), HOST_LOCAL_NODE, 4, &kernel_task);
            VERIFY_PORT(kernel_task, ret);
        }
        if(ret != KERN_SUCCESS)
        {
            DEBUG("Returning failure.");
            return ret;
        }
        DEBUG("Success, caching returned port.");
        initialized = 1;
    }
    DEBUG("kernel_task = 0x%08x", kernel_task);
    *task = kernel_task;
    return KERN_SUCCESS;
}

vm_address_t get_kernel_base()
{
    static vm_address_t addr;
    static char initialized = 0;
    DEBUG("Getting kernel base address...");
    if(initialized)
    {
        DEBUG("Already happened, returning cached value.");
    }
    else
    {
        kern_return_t ret;
        task_t kernel_task;
        vm_region_submap_info_data_64_t info;
        vm_size_t size;
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
        unsigned int depth = 0;

        ret = get_kernel_task(&kernel_task);
        if(ret != KERN_SUCCESS)
        {
            return 0;
        }

        DEBUG("Looping over kernel memory regions...");
        for(addr = 0; 1; addr += size)
        {
            // get next memory region
            DEBUG("Searching for next region at " ADDR "...", addr);
            ret = vm_region_recurse_64(kernel_task, &addr, &size, &depth, (vm_region_info_t)&info, &info_count);
            if(ret != KERN_SUCCESS)
            {
                DEBUG("None found, returning 0.");
                return 0;
            }
            DEBUG("Found region " ADDR "-" ADDR "with %c%c%c", addr, addr + size, (info.protection) & VM_PROT_READ ? 'r' : '-', (info.protection) & VM_PROT_WRITE ? 'w' : '-', (info.protection) & VM_PROT_EXECUTE ? 'x' : '-');

            // the kernel maps over a GB of RAM at the address where it maps itself, and that region has rwx set to ---.
            // we can use those two facts to locate it.
            if(size > 1024*1024*1024 && (info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) == 0)
            {
                DEBUG("Found a matching memory region.");
                // now we have to determine the mach header offset from the beginning of the region.
                // for iOS <= 8 this has been 0x1000 for 32bit and 0x2000 for 64bit.
                // starting with iOS 9, 64bit has shifted to 0x4000 and 32bit idk, but probably 0x2000.
                // so we just check both of those offsets for a possible mach header.
                mach_hdr_t hdr1, hdr2;

                while(true)
                {
                    DEBUG("Reading out Mach-O header from offset 0x%x...", IMAGE_OFFSET);
                    ret = vm_read_overwrite(kernel_task, addr + IMAGE_OFFSET, sizeof(mach_hdr_t), (vm_address_t)&hdr1, &size);
                    if(ret != KERN_SUCCESS)
                    {
                        DEBUG("Failed, returning 0.");
                        return 0;
                    }

                    DEBUG("Reading out Mach-O header from offset 0x%x...", 2 * IMAGE_OFFSET);
                    ret = vm_read_overwrite(kernel_task, addr + 2 * IMAGE_OFFSET, sizeof(mach_hdr_t), (vm_address_t)&hdr2, &size);
                    if(ret != KERN_SUCCESS)
                    {
                        DEBUG("Failed, checking if first header is valid...");
                        // if the second address cannot be read, the first one might still be valid
                        if(hdr1.magic == MACH_HEADER_MAGIC)
                        {
                            DEBUG("Yep, going with offset 0x%x.", IMAGE_OFFSET);
                            addr += IMAGE_OFFSET;
                        }
                        // or not
                        else
                        {
                            DEBUG("Nope, returning 0.");
                            return 0;
                        }
                    }
                    else
                    {
                        DEBUG("Read out two structures successfully, now see which one is valid...");
                        char b1, b2;
                        // we only have a problem if either both or none of the headers have the correct magic
                        b1 = hdr1.magic == MACH_HEADER_MAGIC;
                        b2 = hdr2.magic == MACH_HEADER_MAGIC;
                        if(b1 && b2)
                        {
                            DEBUG("Both of them, that is bad.");
                            // dig a little deeper
                            DEBUG("Checking which one has valid file type and target CPU...");
                            b1 = hdr1.cputype == MACH_TYPE && hdr1.filetype == MH_EXECUTE;
                            b2 = hdr2.cputype == MACH_TYPE && hdr2.filetype == MH_EXECUTE;
                            if(b1 && b2)
                            {
                                // go die in a fire
                                DEBUG("Both of them, returning 0.");
                                DEBUG("Your kernel seems to be at both possible base addresses.");
                                DEBUG("Try rebooting your device, and if the issue persists please open a ticket at:");
                                DEBUG(BUGTRACKER_URL);
                                return 0;
                            }
                        }

                        if(b1)
                        {
                            DEBUG("The first one, going with offset 0x%x.", IMAGE_OFFSET);
                            addr += IMAGE_OFFSET;
                        }
                        else if(b2)
                        {
                            DEBUG("The second one, going with offset 0x%x.", 2 * IMAGE_OFFSET);
                            addr += 2 * IMAGE_OFFSET;
                        }
                        else // no magic match
                        {
                            DEBUG("Neither, going 0x100000 further...");
                            addr += 0x100000;
                            continue; // avoid the break at the end
                        }
                    }
                    break;
                }

                DEBUG("Got kernel base address, caching it.");
                initialized = 1;
                break;
            }
        }
    }
    DEBUG("kernel_base = " ADDR, addr);
    return addr;
}

vm_size_t read_kernel(vm_address_t addr, vm_size_t size, unsigned char* buf)
{
    DEBUG("Reading kernel bytes " ADDR "-" ADDR, addr, addr + size);
    kern_return_t ret;
    task_t kernel_task;
    vm_size_t remainder = size,
              bytes_read = 0;

    ret = get_kernel_task(&kernel_task);
    if(ret != KERN_SUCCESS)
    {
        return -1;
    }

    // reading memory in big chunks seems to cause problems, so
    // we are splitting it up into multiple smaller chunks here
    for(vm_address_t end = addr + size; addr < end; remainder -= size)
    {
        size = remainder > MAX_CHUNK_SIZE ? MAX_CHUNK_SIZE : remainder;
        ret = vm_read_overwrite(kernel_task, addr, size, (vm_address_t)(buf + bytes_read), &size);
        if(ret != KERN_SUCCESS || size == 0)
        {
            break;
        }
        bytes_read += size;
        addr += size;
    }

    return bytes_read;
}

vm_size_t write_kernel(vm_address_t addr, unsigned char* data, vm_size_t size)
{
    DEBUG("Writing to kernel at " ADDR "-" ADDR, addr, addr + size);
    kern_return_t ret;
    task_t kernel_task;
    vm_size_t remainder = size;
    vm_size_t bytes_written = 0;

    ret = get_kernel_task(&kernel_task);
    if(ret != KERN_SUCCESS)
    {
        return -1;
    }

    for(vm_address_t end = addr + size; addr < end; remainder -= size)
    {
        size = remainder > MAX_CHUNK_SIZE ? MAX_CHUNK_SIZE : remainder;
        ret = vm_write(kernel_task, addr, (vm_offset_t)(data + bytes_written), size);
        if (ret != KERN_SUCCESS)
        {
            break;
        }
        bytes_written += size;
        addr += size;
    }

    return bytes_written;
}

vm_address_t find_bytes_kern(vm_address_t start, vm_address_t end, unsigned char* bytes, size_t length)
{
    vm_address_t ret = 0;
    unsigned char* buf = malloc(end - start);
    if(buf)
    {
        // TODO reading in chunks would probably be better
        if(read_kernel(start, end - start, buf))
        {
            void* addr = memmem(buf, end - start, bytes, length);
            if(addr)
            {
                ret = (vm_address_t)addr - (vm_address_t)buf + start;
            }
        }
        free(buf);
    }
    return ret;
}
