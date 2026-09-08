Shared confinement logic

Nothing is built here. The two sources in this directory are the parts of
the confinement machinery that touch no kernel state -- promise parsing,
unveil path matching, and the BPF verifier and interpreter -- and both the
kernel and libroot compile them into themselves. Keeping them in one place
is what stops a program's idea of what a promise name means from drifting
away from the kernel's.
