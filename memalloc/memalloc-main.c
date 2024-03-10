/* General headers */
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <linux/vmalloc.h>
#include <asm/pgalloc.h>

/* File IO-related headers */
#include <linux/fs.h>
#include <linux/bio.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>

/* Sleep and timer headers */
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <linux/pci.h>

#include "../common.h"

/* Simple licensing stuff */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("Project 2, CSE 330 Spring 2024");
MODULE_VERSION("0.01");

/* Calls which start and stop the ioctl teardown */
bool memalloc_ioctl_init(void);
void memalloc_ioctl_teardown(void);

/* Project 2 Solution Variable/Struct Declarations */
#define MAX_PAGES           4096
#define MAX_ALLOCATIONS     100

// Module variables
static dev_t memalloc_dev_number; // Device number
static struct cdev memalloc_cdev;  // Character device structure
static struct allocation_info allocations[MAX_ALLOCATIONS]; // Array to track allocations
static unsigned int num_allocations = 0; // Current number of allocations


/* Page table allocation helper functions defined in kmod_helper.c */
pud_t*  memalloc_pud_alloc(p4d_t* p4d, unsigned long vaddr);
pmd_t*  memalloc_pmd_alloc(pud_t* pud, unsigned long vaddr);
void    memalloc_pte_alloc(pmd_t* pmd, unsigned long vaddr);

#if defined(CONFIG_X86_64)
    #define PAGE_PERMS_RW		PAGE_SHARED
    #define PAGE_PERMS_R		PAGE_READONLY
#else
    #define PAGE_PERMS_RW		__pgprot(_PAGE_DEFAULT | PTE_USER | PTE_NG | PTE_PXN | PTE_UXN | PTE_WRITE)
    #define PAGE_PERMS_R		__pgprot(_PAGE_DEFAULT | PTE_USER | PTE_NG | PTE_PXN | PTE_UXN | PTE_RDONLY)
#endif

struct alloc_info           alloc_req;
struct free_info            free_req;

/* Init and Exit functions */
static int __init memalloc_module_init(void) {
    printk("Hello from the memalloc module!\n");
   // 1. Device Registration
    if (alloc_chrdev_region(&memalloc_dev_number, 0, 1, "memalloc") < 0) {
        return -1; 
    }

    // 2. Initialize Character Device 
    cdev_init(&memalloc_cdev, &memalloc_fops);

    // 3. Add Character Device
    if (cdev_add(&memalloc_cdev, memalloc_dev_number, 1) < 0) {
        unregister_chrdev_region(memalloc_dev_number, 1);
        return -1; 
    }

    // 4. (Optional) Create a Class
    memalloc_class = class_create(THIS_MODULE, "memalloc"); 
    if (IS_ERR(memalloc_class)) {
        cdev_del(&memalloc_cdev);
        unregister_chrdev_region(memalloc_dev_number, 1);
        return PTR_ERR(memalloc_class);
    }

    // 5. Create Device Node
    device_create(memalloc_class, NULL, memalloc_dev_number, NULL, "memalloc");
    if (IS_ERR(device_create)) { 
        class_destroy(memalloc_class);
        cdev_del(&memalloc_cdev);
        unregister_chrdev_region(memalloc_dev_number, 1);
        return PTR_ERR(device_create);
    }

    return 0; 
}

static void __exit memalloc_module_exit(void) {
    /* Teardown IOCTL */
    // 1. Remove the device node
    device_destroy(memalloc_class, memalloc_dev_number);

    // 2. (Optional) Destroy the class
    class_destroy(memalloc_class);

    // 3. Delete the character device
    cdev_del(&memalloc_cdev);

    // 4. Unregister the device number region
    unregister_chrdev_region(memalloc_dev_number, 1);


    printk("Goodbye from the memalloc module!\n");
}

// Function to check if a given address range is already allocated
static int is_address_allocated(unsigned long start_address, unsigned int num_pages) {
    int i;
    for (i = 0; i < num_allocations; i++) {
        struct allocation_info *alloc = &allocations[i];
        if (alloc->start_address == start_address &&
            alloc->num_pages == num_pages) {
            return 1; // Address range is already allocated
        }
    }
    return 0; // Address range is not allocated
}

// Function to allocate pages
static int allocate_pages(struct alloc_info *alloc) {
    // Check if allocation limit reached
    if (num_allocations >= MAX_ALLOCATIONS) {
        return -3; // Allocation limit exceeded
    }

    // Check if address range is already allocated
    if (is_address_allocated(alloc->vaddr, alloc->num_pages)) {
        return -1; // Address range already allocated
    }

    // Allocate pages and update allocation info
    unsigned long addr = alloc->vaddr;
    unsigned int i;
    for (i = 0; i < alloc->num_pages; i++) {
        void *page = (void *)get_zeroed_page(GFP_KERNEL);
        if (!page) {
            return -ENOMEM; // Allocation failed
        }
        unsigned long paddr = __pa(page);
        set_pte_at(current->mm, addr, pfn_pte(paddr >> PAGE_SHIFT, PAGE_PERMS_RW));
        addr += PAGE_SIZE;
    }

    // Update allocation tracking
    allocations[num_allocations].start_address = alloc->vaddr;
    allocations[num_allocations].num_pages = alloc->num_pages;
    num_allocations++;

    return 0; // Allocation successful
}

// Function to free pages
static int free_pages(struct free_info *free) {
    // Find the allocation to free
    int i;
    for (i = 0; i < num_allocations; i++) {
        struct allocation_info *alloc = &allocations[i];
        if (alloc->start_address == free->vaddr &&
            alloc->num_pages == 1) {
            // Free pages and remove allocation from tracking
            unsigned long addr = alloc->start_address;
            unsigned int j;
            for (j = 0; j < alloc->num_pages; j++) {
                clear_pte_at(current->mm, addr);
                addr += PAGE_SIZE;
            }
            // Remove allocation from tracking
            if (i < num_allocations - 1) {
                allocations[i] = allocations[num_allocations - 1];
            }
            num_allocations--;
            return 0; // Free successful
        }
    }

    return -1; // Allocation not found
}

/* IOCTL handler for vmod. */
static long memalloc_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    switch (cmd)
    {
    case ALLOCATE:    	
        /* allocate a set of pages */
        struct alloc_info alloc;
            if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc))) {
                return -EFAULT; // Error copying ALLOCATE data
            }
            return allocate_pages(&alloc); // Allocate pages
        printk("IOCTL: alloc(%lx, %d, %d)\n", alloc_req.vaddr, alloc_req.num_pages, alloc_req.write);
        break;
    case FREE:    	
        /* free allocated pages */
        struct free_info free;
            if (copy_from_user(&free, (void __user *)arg, sizeof(free))) {
                return -EFAULT; // Error copying FREE data
            }
            return free_pages(&free); // Free pages
    	printk("IOCTL: free(%lx)\n", free_req.vaddr);
    	break;    	
    default:
        printk("Error: incorrect IOCTL command.\n");
        return -EINVAL;
    }
    return 0;
}

/* Required file ops. */
static struct file_operations fops = 
{
    .owner          = THIS_MODULE,
    .unlocked_ioctl = memalloc_ioctl,
};

/* Initialize the module for IOCTL commands */
bool memalloc_ioctl_init(void) {
    // Request device number allocation
    if (alloc_chrdev_region(&memalloc_dev_number, 0, 1, "memalloc") < 0) {
        return -1; // Failed to allocate device number
    }

    cdev_init(&memalloc_cdev, &memalloc_fops);
    if (cdev_add(&memalloc_cdev, memalloc_dev_number, 1) < 0) {
        unregister_chrdev_region(memalloc_dev_number, 1);
        return -1; // Failed to add character device
    }

    printk(KERN_INFO "memalloc: Module loaded successfully\n");
    return 0; // Success
}

void memalloc_ioctl_teardown(void) {
    /* Destroy the classes too (IOCTL-specific). */
    cdev_del(&memalloc_cdev);
    unregister_chrdev_region(memalloc_dev_number, 1);
    printk(KERN_INFO "memalloc: Module unloaded\n");
}

module_init(memalloc_module_init);
module_exit(memalloc_module_exit);