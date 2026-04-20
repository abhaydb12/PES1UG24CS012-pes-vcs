1A
<img width="1288" height="800" alt="image" src="https://github.com/user-attachments/assets/cdde3836-fb38-46d0-826f-0e2cd22f79eb" />


1B
<img width="1288" height="800" alt="image" src="https://github.com/user-attachments/assets/0d727c72-d2c0-42d1-859d-1a9ea9ecfed7" />

2A
<img width="1288" height="800" alt="image" src="https://github.com/user-attachments/assets/629f5035-5c4c-4026-8707-acb3965d7be2" />

2B
<img width="1288" height="800" alt="image" src="https://github.com/user-attachments/assets/91757cb9-c372-4614-b0af-0936a6c5bfe5" />

3A
<img width="1288" height="800" alt="image" src="https://github.com/user-attachments/assets/39ec57af-f579-4af0-954b-c6d489ec2b41" />

3B
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/c980b315-6e45-4028-babc-703b895564bc" />

4A
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/8dc1c19e-88fb-4cd8-88e5-9547b2c25c1b" />

4B
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/02595371-4f39-4312-b853-bc4b27c8eb64" />

4C
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/8bdc7263-cec5-4fd8-b445-f0a5c43c65e2" />

Code Files required:

object.c Object store implementation tree.c Tree serialization and construction index.c Staging area implementation commit.c Commit creation and history walking ARE PART OF THE REPO

Analysis Questions (Phase 5 and Phase 6):

Q5.1: A checkout basically rewires the repo to another snapshot. First you read the branch pointer from .pes/refs/heads/<branch> to get the commit hash, then you load that commit and its root tree. .pes/HEAD must be updated to point to the new branch. After that, the working directory has to be completely reshaped to match that tree: files not in the target branch get deleted, missing files are recreated from blob objects, and existing ones are overwritten with the correct content. The tricky part is that you are basically doing a controlled “wipe and rebuild” of the filesystem while making sure you don’t destroy uncommitted work accidentally, and keeping HEAD, index, and disk all in sync without corruption.

Q5.2: Dirty checkout detection is basically a safety scan before switching branches. You take every tracked file from the index and compare it against the working directory (mtime/size or hash) to see if it has local edits. Then you also compare it against the target branch’s version from its commit tree. If a file is modified locally AND also different in the target branch, that’s a conflict because checkout would overwrite it. Same goes for staged changes in the index that would be lost. So the system refuses checkout if any tracked file has unsaved or staged changes that would be overwritten by applying the new branch tree.

Q5.3: Detached HEAD is like checking out a specific commit directly instead of a branch label. If you commit in this state, Git still creates commits normally, but there is no branch pointer moving forward, so your commits are not “owned” by any branch. They become dangling once you switch away. You can recover them if you still know the commit hash, or via reflog (history of HEAD movements), then you create a new branch pointing to that commit to “save” it. Otherwise they just sit in object storage until garbage collection removes them.

Q6.1: Garbage collection works by marking everything reachable first, then deleting the rest. You start from all branch tips in .pes/refs/heads/, push those commits into a stack/queue, and traverse backwards through parents, then down into trees and blobs. Every visited object hash goes into a hash set (this is important for fast lookup and avoiding revisits). After traversal, you scan .pes/objects/ and delete anything not in the set. With 100,000 commits and 50 branches, you don’t necessarily visit all objects—only reachable history—but in worst case (dense shared history) you might still touch ~100k commits plus all their associated trees/blobs, so total could easily reach a few hundred thousand objects depending on repo size.

Q6.2: The danger is timing. Imagine GC starts and sees an object (say a new tree) that hasn’t been linked into a commit yet, so it looks unreachable and deletes it. At the same time, a commit is being created that plans to reference that tree. Now you’ve got a commit pointing to something that no longer exists. Git avoids this by making object creation and reference updates atomic: objects are written first, then refs are updated last. GC also only deletes objects that have been unreachable for a safe period and while holding locks on refs, so it never interferes with in-progress commits or recently created objects.
