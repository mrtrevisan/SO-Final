#ifndef CONTR_H
#define CONTR_H

// contr
// controla o hardware simulado
// em especial, contém o laço de execução de instruções e verificação de
//   interrupções, com chamada ao SO para tratá-las
// concentra os dispositivos de hardware

//tam do init = 10  + tam do p1 = 32  +  tam do p2 = 32  +  tam do a1 = 300 ==   tam total = 374 
//mem folgada = 180
//mem apertada = 75
#define MEM_SEC_TAM 300      // tamanho da "memoria secundaria"
#define MEM_TAM 75         // tamanho da memória principal
#define PAG_TAM 15           // tamanho da pagina

typedef struct contr_t contr_t;

#include "mem.h"
#include "mmu.h"
#include "so.h"
#include "exec.h"
#include "rel.h"

contr_t *contr_cria(void);
void contr_destroi(contr_t *self);

// o laço principal da simulação
void contr_laco(contr_t *self);

// informa ao controlador quem é o SO
void contr_informa_so(contr_t *self, so_t *so);

// funções de acesso aos componentes do hardware
mem_t *contr_mem(contr_t *self);
mmu_t *contr_mmu(contr_t *self);
exec_t *contr_exec(contr_t *self);
es_t *contr_es(contr_t *self);
rel_t *contr_rel(contr_t *self);

#endif // CONTR_H
