/**************************************************************************
 * 
 * This kernel module is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 **************************************************************************/
/*
 * This code provides access to unexported mm kernel features. It is necessary
 * to use the new DRM memory manager code with kernels that don't support it
 * directly.
 *
 * Authors: Thomas Hellstrom <thomas-at-tungstengraphics-dot-com>
 *          Linux kernel mm subsystem authors. 
 *          (Most code taken from there).
 */

#include "drmP.h"

#if defined(CONFIG_X86) && (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15))

/*
 * These have bad performance in the AGP module for the indicated kernel versions.
 */

int drm_map_page_into_agp(struct page *page)
{
        int i;
        i = change_page_attr(page, 1, PAGE_KERNEL_NOCACHE);
        /* Caller's responsibility to call global_flush_tlb() for
         * performance reasons */
        return i;
}

int drm_unmap_page_from_agp(struct page *page)
{
        int i;
        i = change_page_attr(page, 1, PAGE_KERNEL);
        /* Caller's responsibility to call global_flush_tlb() for
         * performance reasons */
        return i;
}
#endif 


#if  (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))

/*
 * The protection map was exported in 2.6.19
 */

pgprot_t vm_get_page_prot(unsigned long vm_flags)
{
#ifdef MODULE
	static pgprot_t drm_protection_map[16] = {
		__P000, __P001, __P010, __P011, __P100, __P101, __P110, __P111,
		__S000, __S001, __S010, __S011, __S100, __S101, __S110, __S111
	};

	return drm_protection_map[vm_flags & 0x0F];
#else
	extern pgprot_t protection_map[];
	return protection_map[vm_flags & 0x0F];
#endif
};
#endif


#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15))

/*
 * vm code for kernels below 2.6.15 in which version a major vm write
 * occured. This implement a simple straightforward 
 * version similar to what's going to be
 * in kernel 2.6.19+
 * Kernels below 2.6.15 use nopage whereas 2.6.19 and upwards use
 * nopfn.
 */ 

static struct {
	spinlock_t lock;
	struct page *dummy_page;
	atomic_t present;
} drm_np_retry = 
{SPIN_LOCK_UNLOCKED, NOPAGE_OOM, ATOMIC_INIT(0)};

struct page * get_nopage_retry(void)
{
	if (atomic_read(&drm_np_retry.present) == 0) {
		struct page *page = alloc_page(GFP_KERNEL);
		if (!page)
			return NOPAGE_OOM;
		spin_lock(&drm_np_retry.lock);
		drm_np_retry.dummy_page = page;
		atomic_set(&drm_np_retry.present,1);
		spin_unlock(&drm_np_retry.lock);
	}
	get_page(drm_np_retry.dummy_page);
	return drm_np_retry.dummy_page;
}

void free_nopage_retry(void)
{
	if (atomic_read(&drm_np_retry.present) == 1) {
		spin_lock(&drm_np_retry.lock);
		__free_page(drm_np_retry.dummy_page);
		drm_np_retry.dummy_page = NULL;
		atomic_set(&drm_np_retry.present, 0);
		spin_unlock(&drm_np_retry.lock);
	}
}

struct page *drm_bo_vm_nopage(struct vm_area_struct *vma,
			       unsigned long address, 
			       int *type)
{
	struct fault_data data;

	if (type)
		*type = VM_FAULT_MINOR;

	data.address = address;
	data.vma = vma;
	drm_bo_vm_fault(vma, &data);
	switch (data.type) {
	case VM_FAULT_OOM:
		return NOPAGE_OOM;
	case VM_FAULT_SIGBUS:
		return NOPAGE_SIGBUS;
	default:
		break;
	}

	return NOPAGE_REFAULT;
}

#endif

#if !defined(DRM_FULL_MM_COMPAT) && \
  ((LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)) || \
   (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19)))

static int drm_pte_is_clear(struct vm_area_struct *vma,
			    unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	int ret = 1;
	pte_t *pte;
	pmd_t *pmd;
	pud_t *pud;
	pgd_t *pgd;

	spin_lock(&mm->page_table_lock);
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		goto unlock;
	pud = pud_offset(pgd, addr);
        if (pud_none(*pud))
		goto unlock;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		goto unlock;
	pte = pte_offset_map(pmd, addr);
	if (!pte)
		goto unlock;
	ret = pte_none(*pte);
	pte_unmap(pte);
 unlock:
	spin_unlock(&mm->page_table_lock);
	return ret;
}

int vm_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
		  unsigned long pfn)
{
	int ret;
	if (!drm_pte_is_clear(vma, addr))
		return -EBUSY;

	ret = io_remap_pfn_range(vma, addr, pfn, PAGE_SIZE, vma->vm_page_prot);
	return ret;
}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,19) && !defined(DRM_FULL_MM_COMPAT))

/**
 * While waiting for the fault() handler to appear in
 * we accomplish approximately
 * the same wrapping it with nopfn.
 */

unsigned long drm_bo_vm_nopfn(struct vm_area_struct * vma,
			   unsigned long address)
{
	struct fault_data data;
	data.address = address;

	(void) drm_bo_vm_fault(vma, &data);
	if (data.type == VM_FAULT_OOM)
		return NOPFN_OOM;
	else if (data.type == VM_FAULT_SIGBUS)
		return NOPFN_SIGBUS;

	/*
	 * pfn already set.
	 */

	return 0;
}
#endif


#ifdef DRM_ODD_MM_COMPAT

/*
 * VM compatibility code for 2.6.15-2.6.18. This code implements a complicated
 * workaround for a single BUG statement in do_no_page in these versions. The
 * tricky thing is that we need to take the mmap_sem in exclusive mode for _all_
 * vmas mapping the ttm, before dev->struct_mutex is taken. The way we do this is to 
 * check first take the dev->struct_mutex, and then trylock all mmap_sems. If this
 * fails for a single mmap_sem, we have to release all sems and the dev->struct_mutex,
 * release the cpu and retry. We also need to keep track of all vmas mapping the ttm.
 * phew.
 */

typedef struct p_mm_entry {
	struct list_head head;
	struct mm_struct *mm;
	atomic_t refcount;
        int locked;
} p_mm_entry_t;

typedef struct vma_entry {
	struct list_head head;
	struct vm_area_struct *vma;
} vma_entry_t;


struct page *drm_bo_vm_nopage(struct vm_area_struct *vma,
			       unsigned long address, 
			       int *type)
{
	drm_buffer_object_t *bo = (drm_buffer_object_t *) vma->vm_private_data;
	unsigned long page_offset;
	struct page *page;
	drm_ttm_t *ttm; 
	drm_device_t *dev;

	mutex_lock(&bo->mutex);

	if (type)
		*type = VM_FAULT_MINOR;

	if (address > vma->vm_end) {
		page = NOPAGE_SIGBUS;
		goto out_unlock;
	}
	
	dev = bo->dev;

	if (drm_mem_reg_is_pci(dev, &bo->mem)) {
		DRM_ERROR("Invalid compat nopage.\n");
		page = NOPAGE_SIGBUS;
		goto out_unlock;
	}

	ttm = bo->ttm;
	drm_ttm_fixup_caching(ttm);
	page_offset = (address - vma->vm_start) >> PAGE_SHIFT;
	page = drm_ttm_get_page(ttm, page_offset);
	if (!page) {
		page = NOPAGE_OOM;
		goto out_unlock;
	}

	get_page(page);
out_unlock:
	mutex_unlock(&bo->mutex);
	return page;
}




int drm_bo_map_bound(struct vm_area_struct *vma)
{
	drm_buffer_object_t *bo = (drm_buffer_object_t *)vma->vm_private_data;
	int ret = 0;
	unsigned long bus_base;
	unsigned long bus_offset;
	unsigned long bus_size;
	
	ret = drm_bo_pci_offset(bo->dev, &bo->mem, &bus_base, 
				&bus_offset, &bus_size);
	BUG_ON(ret);

	if (bus_size) {
		drm_mem_type_manager_t *man = &bo->dev->bm.man[bo->mem.mem_type];
		unsigned long pfn = (bus_base + bus_offset) >> PAGE_SHIFT;
		pgprot_t pgprot = drm_io_prot(man->drm_bus_maptype, vma);
		ret = io_remap_pfn_range(vma, vma->vm_start, pfn,
					 vma->vm_end - vma->vm_start,
					 pgprot);
	}

	return ret;
}
	

int drm_bo_add_vma(drm_buffer_object_t * bo, struct vm_area_struct *vma)
{
	p_mm_entry_t *entry, *n_entry;
	vma_entry_t *v_entry;
	struct mm_struct *mm = vma->vm_mm;

	v_entry = drm_ctl_alloc(sizeof(*v_entry), DRM_MEM_BUFOBJ);
	if (!v_entry) {
		DRM_ERROR("Allocation of vma pointer entry failed\n");
		return -ENOMEM;
	}
	v_entry->vma = vma;

	list_add_tail(&v_entry->head, &bo->vma_list);

	list_for_each_entry(entry, &bo->p_mm_list, head) {
		if (mm == entry->mm) {
			atomic_inc(&entry->refcount);
			return 0;
		} else if ((unsigned long)mm < (unsigned long)entry->mm) ;
	}

	n_entry = drm_ctl_alloc(sizeof(*n_entry), DRM_MEM_BUFOBJ);
	if (!n_entry) {
		DRM_ERROR("Allocation of process mm pointer entry failed\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&n_entry->head);
	n_entry->mm = mm;
	n_entry->locked = 0;
	atomic_set(&n_entry->refcount, 0);
	list_add_tail(&n_entry->head, &entry->head);

	return 0;
}

void drm_bo_delete_vma(drm_buffer_object_t * bo, struct vm_area_struct *vma)
{
	p_mm_entry_t *entry, *n;
	vma_entry_t *v_entry, *v_n;
	int found = 0;
	struct mm_struct *mm = vma->vm_mm;

	list_for_each_entry_safe(v_entry, v_n, &bo->vma_list, head) {
		if (v_entry->vma == vma) {
			found = 1;
			list_del(&v_entry->head);
			drm_ctl_free(v_entry, sizeof(*v_entry), DRM_MEM_BUFOBJ);
			break;
		}
	}
	BUG_ON(!found);

	list_for_each_entry_safe(entry, n, &bo->p_mm_list, head) {
		if (mm == entry->mm) {
			if (atomic_add_negative(-1, &entry->refcount)) {
				list_del(&entry->head);
				BUG_ON(entry->locked);
				drm_ctl_free(entry, sizeof(*entry), DRM_MEM_BUFOBJ);
			}
			return;
		}
	}
	BUG_ON(1);
}



int drm_bo_lock_kmm(drm_buffer_object_t * bo)
{
	p_mm_entry_t *entry;
	int lock_ok = 1;
	
	list_for_each_entry(entry, &bo->p_mm_list, head) {
		BUG_ON(entry->locked);
		if (!down_write_trylock(&entry->mm->mmap_sem)) {
			lock_ok = 0;
			break;
		}
		entry->locked = 1;
	}

	if (lock_ok)
		return 0;

	list_for_each_entry(entry, &bo->p_mm_list, head) {
		if (!entry->locked) 
			break;
		up_write(&entry->mm->mmap_sem);
		entry->locked = 0;
	}

	/*
	 * Possible deadlock. Try again. Our callers should handle this
	 * and restart.
	 */

	return -EAGAIN;
}

void drm_bo_unlock_kmm(drm_buffer_object_t * bo)
{
	p_mm_entry_t *entry;
	
	list_for_each_entry(entry, &bo->p_mm_list, head) {
		BUG_ON(!entry->locked);
		up_write(&entry->mm->mmap_sem);
		entry->locked = 0;
	}
}

int drm_bo_remap_bound(drm_buffer_object_t *bo) 
{
	vma_entry_t *v_entry;
	int ret = 0;

	if (drm_mem_reg_is_pci(bo->dev, &bo->mem)) {
		list_for_each_entry(v_entry, &bo->vma_list, head) {
			ret = drm_bo_map_bound(v_entry->vma);
			if (ret)
				break;
		}
	}

	return ret;
}

void drm_bo_finish_unmap(drm_buffer_object_t *bo)
{
	vma_entry_t *v_entry;

	list_for_each_entry(v_entry, &bo->vma_list, head) {
		v_entry->vma->vm_flags &= ~VM_PFNMAP; 
	}
}	

#endif

