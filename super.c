#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
#include <linux/blkdev.h>

#include "babyfs.h"

int NR_DSTORE_BLOCKS;
struct super_operations babyfs_super_opts;

static int babyfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct buffer_head *bh;
  struct baby_super_block *baby_sb;
  struct baby_sb_info *baby_sb_info;
  struct inode *root_vfs_inode;
  long ret = -ENOMEM;

  if (!(baby_sb_info = kzalloc(sizeof(*baby_sb_info), GFP_KERNEL))) {
    printk(KERN_ERR "babyfs_fill_super: kalloc baby_sb_info failed!\n");
    goto failed;
  }

  if (!sb_set_blocksize(sb, BABYFS_BLOCK_SIZE)) { // 设置 sb_bread 读取的逻辑块大小
    printk(KERN_ERR "sb_set_blocksize: failed! current blocksize: %lu\n",
           sb->s_blocksize);
    goto failed;
  }

  // 仅在初始化的时候读取超级块，后面只需要操作内存中的超级块对象，并在恰当的时候同步到磁盘
  if (!(bh = sb_bread(sb, BABYFS_SUPER_BLOCK))) {
    printk(KERN_ERR "babyfs_fill_super: canot read super block\n");
    goto failed;
  }
  baby_sb = (struct baby_super_block *)bh->b_data;
  NR_DSTORE_BLOCKS = baby_sb->nr_dstore_blocks;

  // 初始化超级块
  sb->s_magic = baby_sb->magic; // 魔幻数
  sb->s_op = &babyfs_super_opts; // 操作集合
  baby_sb_info->s_babysb = baby_sb;
  baby_sb_info->s_sbh = bh;
  sb->s_fs_info = baby_sb_info; // superblock 的私有域存放额外信息，包括磁盘上的结构体

  // 获取磁盘存储的 inode 结构体
  root_vfs_inode = baby_iget(sb, BABYFS_ROOT_INODE_NO);
  if (IS_ERR(root_vfs_inode)) {
		ret = PTR_ERR(root_vfs_inode);
	  goto failed_mount;
	}

  // 创建根目录
  sb->s_root = d_make_root(root_vfs_inode);
  if (!sb->s_root) {
    printk(KERN_ERR "babyfs_fill_super: create root dentry failed\n");
    ret = -ENOMEM;
    goto failed_mount;
  }
  return 0;

failed_mount:
  brelse(bh);
failed:  
  return ret;
}

static struct dentry *babyfs_mount(struct file_system_type *fs_type, int flags,
                                   const char *dev_name, void *data) {
  // 在块设备上挂载文件系统
  struct dentry *dentry =
      mount_bdev(fs_type, flags, dev_name, data, babyfs_fill_super);
  if (!dentry) printk(KERN_ERR "babyfs_mount: mounted error\n");
  return dentry;
}

// baby_inode_info 内存高速缓存（slab层），其中包含 vfs inode，相当于 vfs
// inode，相当于 的扩充 inode
// 结构体是磁盘索引节点在内存中的体现，会频繁地创建和释放，因此用 slab
// 分配器来管理很有必要
static struct kmem_cache *baby_inode_cachep;

/**
 * new_inode->alloc_inode->baby_alloc_inode
 * 分配一个 inode 节点，使用自定义的 baby_inode_info slab 代替默认的 inode slab
 */
static struct inode *baby_alloc_inode(struct super_block *sb) {
  struct baby_inode_info *bbi;
  bbi = kmem_cache_alloc(baby_inode_cachep, GFP_KERNEL);
  if (!bbi) return NULL;

  return &bbi->vfs_inode;
}

static void baby_i_callback(struct rcu_head *head) {
  struct inode *inode = container_of(head, struct inode, i_rcu);
  kmem_cache_free(baby_inode_cachep, BABY_I(inode));
}

// 删除一个 vfs inode，需要在 slab 中回收 baby_inode_info 实例
static void baby_destroy_inode(struct inode *inode) {
  call_rcu(&inode->i_rcu, baby_i_callback);
}

static void init_once(void *foo) {
  struct baby_inode_info *bbi = (struct baby_inode_info *)foo;
  // 初始化 baby_inode_info 包含的 vfs inode
  inode_init_once(&bbi->vfs_inode);
}

// 初始化 baby_inode_info 内存高速缓存（slab层）
static int __init init_inodecache(void) {
  /**
   * Linux内核有一个usercopy whitelist机制，只允许这里面的region来做usercopy。
   * 如果是用kmem_cache_create申请的kmem_cache申请的内存空间来copy to user或者
   * copy from user，那么就会报这个错。这时要用kmem_cache_create_usercopy，
   * 来将申请的区域加入到usercopy whitelist中。
   */
  baby_inode_cachep = kmem_cache_create_usercopy(
      "baby_inode_info", sizeof(struct baby_inode_info), 0,
      (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT),
      offsetof(struct baby_inode_info, i_blocks), // usercopy 域
      sizeof_field(struct baby_inode_info, i_blocks), // usercopy 域的大小
      init_once); 
  if (baby_inode_cachep == NULL) return -ENOMEM;
  return 0;
}

// 删除 baby_inode_info 内存高速缓存（slab层）
static void destroy_inodecache(void) {
  /*
   * Make sure all delayed rcu free inodes are flushed before we
   * destroy cache.
   */
  rcu_barrier();
  kmem_cache_destroy(baby_inode_cachep);
}

static void baby_put_super(struct super_block *sb) {
  struct baby_sb_info *baby_sb_info = BABY_SB(sb);
  if (baby_sb_info == NULL) {
    return;
  }
  brelse(baby_sb_info->s_sbh);
  sb->s_fs_info = NULL;
  kfree(baby_sb_info);
}

struct super_operations babyfs_super_opts = { // 自定义 super_block 操作集合
  .statfs       = simple_statfs,        // 给出文件系统的统计信息，例如使用和未使用的数据块的数目，或者文件名的最大长度
  .alloc_inode	= baby_alloc_inode,     // 申请 inode
  .destroy_inode= baby_destroy_inode,   // 释放 inode
	.write_inode	= baby_write_inode,     // 将 inode 写到磁盘上
  .put_super    = baby_put_super,       // 删除超级块实例的方法
};

static struct file_system_type baby_fs_type = { // 文件系统类型
  .owner        = THIS_MODULE,
  .name         = "babyfs",
  .mount        = babyfs_mount,
  .kill_sb      = kill_block_super, // VFS 提供的销毁方法
  .fs_flags     = FS_REQUIRES_DEV,  // 给定文件系统的每个实例都使用底层块设备
};

static int __init init_babyfs(void) {
  int err;

  printk("init babyfs\n");
  // 初始化 baby_inode_info 内存高速缓存（slab层）
  err = init_inodecache();
  if (err) return err;

  // 注册文件系统类型到系统中
  err = register_filesystem(&baby_fs_type);
  if (err) {
    destroy_inodecache();
    return err;
  }

  return 0;
}

static void __exit exit_babyfs(void) {
  printk("unloading fs...\n");
  unregister_filesystem(&baby_fs_type);
	destroy_inodecache();
}

module_init(init_babyfs);
module_exit(exit_babyfs);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("my little baby filesystem");
MODULE_VERSION("Ver 0.1.0");