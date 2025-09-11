b swtch
c
p cpus[$tp]->proc->name
delete
b kernel/sysfile.c:383
c
p cpus[$tp]->proc->name
da