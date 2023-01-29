#include "so.h"
#include "tela.h"
#include "tab_pag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct metricas_proc {
  //metricas principais
  int tempo_retorno;          //tempo entre criação e fim
  int tempo_bloq;             //tempo que passou bloqueado
  int tempo_exec;             //tempo que passou executando
  int tempo_pronto;           //tempo que passou no estado pronto
  int n_bloqs;                //numero de bloqueios por E/S
  int n_preemp;               //numero de bloqueios por preempção
  //metricas auxiliares
  int existe_desde;         //registro do tempo de cpu no momento da criação do proc
  int bloq_desde;           //momento que bloqueou pela ultima vez
  int exec_desde;           //momendo que entrou em exec pela ultima vez
  int pronto_desde;         //momento que desbloqueou pela ultima vez
};

struct metricas_so {
  //metricas principais
  int tempo_total;          //tempo total 
  int tempo_total_exec;     //tempo total em não parado
  int n_int;                //numero de interrupções
  int n_fal_pag;            //numero de falhas de paginação
  //metricas auxiliares
  int exec_desde;           //momento que saiu do modo zumbi pela ultima vez
};

struct proc_t
{
  int id_proc;                // id do processo
  cpu_estado_t *cpue_proc;    // estado da cpu do processo
  proc_estado_t est_proc;     // estado do processo
  bloq_t motivo_bloq;         // pq bloqueou
  int complemento;            // qual disp bloqueou ou qual endereço causou falaha de pag
  float tempo_expect;         //tempo esperado da próxima execução
  float tempo_exec;           //tempo que passou executando desde que recebeu processador

  metricas_proc m_proc;       //metricas do processo
  tab_pag_t* tab_pag;         //tabela de paginas do processo
  mem_t* mem_sec;             //"memoria secundaria" copia da mem do processo
  
  proc_t* prox;               //proximo da fila de processos (bloqueados ou prontos)
};

struct quadro_t {
  int id;
  bool aloc;

  proc_t* proc_dono;          //processo dono do quadro
  int pag;                    //pagina do processo que esta no quadro
};

struct so_t {
  contr_t *contr;         // o controlador do hardware
  bool paniquei;          // apareceu alguma situação intratável
  cpu_estado_t *cpue;     // cópia do estado da CPU

  proc_t* proc_exec;      // ponteiro pro processo atual
  proc_t* proc_bloq;      // inicio da fila de processos bloqueados
  proc_t* proc_prt;       // inicio da fila de processos prontos

  int quantum;                            //quantum
  int (*esc_ptr) (so_t*, proc_t**);       //ponteiro para o escalonador de processos
  quadro_t* (*esc_pag) (so_t*);           //ponteiro para o escalonador de páginas

  int n_quadros;                      //numero de quadros
  quadro_t* quadros;                  //vetor de quadros 
  
  char nome_esc_proc[20];                //nome do escalonador de processos
  char nome_esc_pag[20];                 //nome do escalonador de paginas
  metricas_so m_so;                   //metricas do SO
};

// funções auxiliares
static void init_mem(so_t *self, proc_t* proc);
static void init_mem_p1(so_t *self, proc_t* proc);
static void init_mem_p2(so_t *self, proc_t* proc);
static void init_mem_a1(so_t *self, proc_t* proc);
static void panico(so_t *self);
static void salva_metricas_proc(proc_t* proc);
static void salva_metricas_so(so_t* so);

//############################################ ESCALONADORES DE PÁGINAS ########################################

//escalonador de páginas First-In-First-Out
quadro_t* escalonador_fifo(so_t* self)
{
  //escolhe o primeiro quadro alocado
  quadro_t* primeiro = &(self->quadros[0]);
  proc_t* proc;
  int pag;
  for (int i = 0; i < self->n_quadros; i++){
    proc = self->quadros[i].proc_dono;          //processo dono do quadro a ser comparado
    pag = self->quadros[i].pag;                 //pag referente ao quadro a ser comparado
    //compara as timestamps do momento em que os quadros foram alocados pela ultima vez
    //(em tempos de cpu) o menor é o mais velho
    if (tab_pag_tempo_aloc(proc->tab_pag, pag) < tab_pag_tempo_aloc(primeiro->proc_dono->tab_pag, primeiro->pag))
      primeiro = &(self->quadros[i]);
  }
  return primeiro;
}

//escalonador de páginas Least-Recently-Used
quadro_t* escalonador_lru(so_t* self)
{
  quadro_t* desusado = &(self->quadros[0]);
  proc_t* proc;
  int pag;

  for (int i = 0; i < self->n_quadros; i++){
    proc = self->quadros[i].proc_dono;        //processo dono do quadro a ser comparado
    pag = self->quadros[i].pag;               //pag referente ao quadro a ser comparado
    //compara as timestamps do momento em que os quadros foram acessados pela ultima vez
    //a mmu atualiza esse tempo em mmu_le()
    //(em tempos de cpu) o menor é o menos recente
    if ( tab_pag_ultimo_acesso(proc->tab_pag, pag) < tab_pag_ultimo_acesso(desusado->proc_dono->tab_pag, desusado->pag) )
      desusado = &(self->quadros[i]);
  }
  return desusado;
}

//escolhe um quadro para ser substituído
quadro_t* pega_quadro(so_t* self){
  //procura por quadros não alocados
  for (int i = 0; i < self->n_quadros; i++){
    if (self->quadros[i].aloc == false){
      return &(self->quadros[i]);
    }
  }
  //debug
  //t_printf("chamando escalonador de paginas");
  //se não encontrar nenhum livre, chama o escalonador
  return self->esc_pag(self);
}

//################################################ FUNÇÕES DE PAGINAÇÃO #########################################

//aloca (ou realoca) um quadro para o processo
void aloc_quadro(so_t* self, quadro_t* quadro, proc_t* proc, int pag){
  quadro->aloc = true;
  quadro->proc_dono = proc;
  quadro->pag = pag;
}

//inicializa uma tabela de paginas para o processo
void inicializa_tab_pag(so_t* self, proc_t* proc, int tam){
  //calculo do numero de paginas que o processo precisa
  int n_pag =  tam / PAG_TAM +1;
  proc->tab_pag = tab_pag_cria(n_pag, PAG_TAM);
}

//desaloca todos os quadros do processo
void libera_quadros(so_t* self, proc_t* proc){
  for (int i = 0; i < self->n_quadros; i++){
    if (self->quadros[i].proc_dono == proc){
      self->quadros[i].aloc = false;
    }
  }
}

//escreve mudanças na mem principal na mem secundária
void write_back(so_t* self, quadro_t* quadro){
  proc_t* proc = quadro->proc_dono;
  int pag = quadro->pag;
  int val;
  err_t err;

  if ( tab_pag_alterada(proc->tab_pag, pag) ) {  //se a tabela de paginas foi alterada
    //muda a tabela a ser usada pela mmu
    mmu_usa_tab_pag(contr_mmu(self->contr), proc->tab_pag);
    //le da mem principal e escreve na mem sec, o equivalente a uma página
    for (int i = 0; i < PAG_TAM; i++){
      err = mmu_le(contr_mmu(self->contr), pag*PAG_TAM +i, &val);
      if (err != ERR_OK) {
        t_printf("erro no write back, end %d, erro %s", pag*PAG_TAM +i, err_nome(err));
        panico(self);
      }
      mem_escreve(proc->mem_sec, pag*PAG_TAM +i, val);
      if (err != ERR_OK) {
        t_printf("erro no write back, end %d, erro %s", pag*PAG_TAM +i, err_nome(err));
        panico(self);
      }
    }
  }
}

//carrega a página que o processo precisa para executar
//a partir do endereço que causou falha de páginas
void carrega_pag_proc(so_t* self, int end){
  proc_t* proc = self->proc_exec;
  int val;
  err_t err;

  //calcula a pagina correspondente
  int pag = end / PAG_TAM;
  //pega um quadro para carregar a pagina
  quadro_t* quadro = pega_quadro(self);
  //debug
  //t_printf("achou quadro %d para alocar", quadro->id);

  //se o quadro está alocado, salva as mudanças na mem sec e invalida o a pagina antiga
  if (quadro->aloc == true){
    write_back(self, quadro);
    tab_pag_muda_valida(quadro->proc_dono->tab_pag, quadro->pag, false);
  }
 
  //aloca ou realoca o quadro
  aloc_quadro(self, quadro, proc, pag);
  //atualiza tabela de páginas
  tab_pag_muda_quadro(proc->tab_pag, pag, quadro->id);
  tab_pag_muda_valida(proc->tab_pag, pag, true); 
  //manda a mmu usar a tabela atualizada (caso o write back tenha alterado)
  mmu_usa_tab_pag(contr_mmu(self->contr), proc->tab_pag);

  //carrega a pagina da mem secundaria para mem principal
  for (int i = 0; i < PAG_TAM; i++){
    err = mem_le(proc->mem_sec, pag*PAG_TAM +i, &val);
    if (err != ERR_OK){
      t_printf("erro na paginação, mem_sec, end %d, erro %s", pag*PAG_TAM +i, err_nome(err));
    }
    err = mmu_escreve(contr_mmu(self->contr), pag*PAG_TAM +i, val);
    if (err != ERR_OK){
      t_printf("erro na paginação, mmu, end %d, erro %s", pag*PAG_TAM +i, err_nome(err));
    }
  }
  //atualiza outras entradas de controle
  tab_pag_muda_tempo_aloc(proc->tab_pag, pag, rel_agora(contr_rel(self->contr)));
  tab_pag_muda_acessada(proc->tab_pag, pag, false);
  tab_pag_muda_alterada(proc->tab_pag, pag, false);
  return;
}

//################################################ FUNÇÕES DE ESTADO DE PROCESSO #######################################

//desbloqueia um processo
void desbloqueia_proc(so_t* self, proc_t* proc){
  //debug
  //t_printf("Desbloqueando proc %d", proc->id_proc);

  //metricas
  proc->m_proc.tempo_bloq += rel_agora(contr_rel(self->contr)) - proc->m_proc.bloq_desde;
  proc->m_proc.pronto_desde = rel_agora(contr_rel(self->contr));

  proc->est_proc = PRONTO;

  proc_t* aux = self->proc_prt;
  if (aux == NULL){           //fila de prontos ta vazia
    self->proc_prt = proc;
  }
  if (aux != NULL){
    while (aux->prox != NULL){
      aux = aux->prox;
    }                         //aux = fim da fila de prontos
    aux->prox = proc;         //coloca o processo desbloq no fim da fila de prontos
  } 

  proc_t* aux2 = self->proc_bloq;
  if (aux2->id_proc == proc->id_proc) {       //o processo que desbloqueou é o primeiro da fila
    self->proc_bloq = self->proc_bloq->prox;  //retira o processo da fila de bloqueados
  } else {
    while ((aux2->prox != NULL) && (aux2->prox->id_proc != proc->id_proc)){
      aux2 = aux2->prox;
    }
    aux2->prox = aux2->prox->prox;
  }

  proc->prox = NULL;
}

//bloqueia o processo em execução
void bloqueia_proc(so_t* self, bloq_t motivo, int complemento){
  //debug
  //t_printf("Bloqueando proc %d", self->proc_exec->id_proc);

  //metricas 
  self->proc_exec->m_proc.tempo_exec += rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.exec_desde;
  self->proc_exec->m_proc.bloq_desde = rel_agora(contr_rel(self->contr));
  self->proc_exec->m_proc.n_bloqs++;

  self->proc_exec->est_proc = BLOQ;     
  self->proc_exec->motivo_bloq = motivo;             //guarda o motivo do bloqueio
  self->proc_exec->complemento = complemento;        //guarda qual disp bloqueou

  self->proc_exec->tempo_expect += self->proc_exec->tempo_exec;
  self->proc_exec->tempo_expect /= 2;
  self->proc_exec->tempo_exec = 0.0;          //zera o tempo em que executou

  proc_t* aux = self->proc_bloq;
  if (aux == NULL) {                        //fila de bloqueados esta vazia
    self->proc_bloq = self->proc_exec;      //coloca o processo bloquado na fila 
  } else {
    while(aux->prox != NULL){
      aux = aux->prox;
    }
    aux->prox = self->proc_exec;          //coloca o processo que bloqueou no fim da fila de bloqueados
  }

  proc_t* aux2 = self->proc_prt;
  if (aux2 == self->proc_exec) {            //o processo que bloqueou é o primeiro da fila
    self->proc_prt = self->proc_prt->prox;  //retira o processo da fila de prontos
  } else {
    while ((aux2->prox != NULL) && (aux2->prox->id_proc != self->proc_exec->id_proc)){
      aux2 = aux2->prox;
    }
    aux2->prox = aux2->prox->prox;
  }

  self->proc_exec->prox = NULL;
}

//########################################### ESCALONADORES DE PROCESSOS #######################################

//escalonador Mais-Curto-Primeiro
int escalonador_curto(so_t *self, proc_t** novo){
  //debug
  //t_printf("Chamado escalonador mais curto");
  proc_t* aux = self->proc_prt;

  if (self->proc_prt == NULL){      //todos os processos estão bloqueados... ou
    if (self->proc_bloq == NULL){   //acabaram os processos
      return -1;
    }  
    return 0;
  }
  else {                //tem pelo menos 1 processo pronto
    *novo = aux;
    while(aux->prox != NULL) {
      if (aux->tempo_expect < (*novo)->tempo_expect){ 
        //achou um mais curto
        *novo = aux;
      }
      aux = aux->prox;
    }
    return 1;
  }
}

//escalonador Round-Robin
int escalonador_round(so_t *self, proc_t** novo)
{
  //debug
  //t_printf("Chamado escalonador circular");

  if (self->proc_prt == NULL){      //todos os processos estão bloqueados... ou
    if (self->proc_bloq == NULL){   //acabaram os processos
      return -1;
    }  
    return 0;
  }
  else {
    *novo = self->proc_prt;       //simplesmente pega o primeiro da fila
    return 1;
  }
}

//################################################ FUNÇÕES DE ESCALONAMENTO #######################################

//troca o processo que está executando
void troca_processo(so_t* self){
  proc_t* novo = NULL;
  int esc = (*(self->esc_ptr))(self, &novo);

  //debug
  //if (novo != NULL) t_printf("idx = %d", novo->id_proc);
  //else t_printf("tudo bloq");
  
  if (esc == -1) { //acabaram os processos
    self->proc_exec = NULL;

    //atualiza as metricas
    if (cpue_modo(self->cpue) != zumbi){  //a cpu estava executando
      self->m_so.tempo_total_exec += rel_agora(contr_rel(self->contr)) - self->m_so.exec_desde;
    }
    self->m_so.tempo_total = rel_agora(contr_rel(self->contr));
    
    t_printf("Fim da execução.");
    panico(self);
    return;

  } else if (esc == 0) { //todos bloqueados
    self->proc_exec = NULL;

    //atualiza metricas
    if (cpue_modo(self->cpue) != zumbi){    //cpu está entrando em modo zumbi
      self->m_so.tempo_total_exec += rel_agora(contr_rel(self->contr)) - self->m_so.exec_desde;
    }

    cpue_muda_modo(self->cpue, zumbi);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);
    return;

  } else if (esc == 1) { //achou processo
    self->proc_exec = novo;

    //metricas
    self->proc_exec->m_proc.tempo_pronto += rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.pronto_desde;
    self->proc_exec->m_proc.exec_desde = rel_agora(contr_rel(self->contr));
    if (cpue_modo(self->cpue) == zumbi){    //cpu esta saindo do modo zumbi 
      self->m_so.exec_desde = rel_agora(contr_rel(self->contr));
    }
  
    //troca a tabela de paginas da mmu
    mmu_usa_tab_pag(contr_mmu(self->contr), self->proc_exec->tab_pag);
    //restaura o contexto do processo
    cpue_copia(self->proc_exec->cpue_proc, self->cpue);
    cpue_muda_modo(self->cpue, usuario);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);

    return;
  }
}

//verifica processos tentando desbloqueá-los
void verif_processos(so_t *self)
{
  // processo a ser verificado
  proc_t* proc_verif = self->proc_bloq; //inicio da fila de bloqueados

  while(proc_verif != NULL) { //processo existe
      if (proc_verif->motivo_bloq == rel) //motivo do bloq foi o relogio
      {
        desbloqueia_proc(self, proc_verif);
      }
      else if (es_pronto(contr_es(self->contr), proc_verif->complemento, (acesso_t)proc_verif->motivo_bloq)) //motivo foi E/S
      { // ve se pode desbloquear
        desbloqueia_proc(self, proc_verif);
      }
    proc_verif = proc_verif->prox; //proximo processo
  }
}

//################################################ FUNÇÕES DE CRIAÇÃO E INICIALIZAÇÃO #######################################

//cria um novo processo
proc_t* novo_proc(so_t* self, int id)
{
  proc_t* novo = malloc(sizeof(*novo));
  if (novo == NULL) return NULL;
  
  novo->cpue_proc = cpue_cria();
  novo->mem_sec = mem_cria(MEM_SEC_TAM);

  novo->id_proc = id;
  novo->est_proc = PRONTO;

  novo->tempo_expect = self->quantum;
  novo->tempo_exec = 0.0;

  novo->m_proc = (metricas_proc){0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  novo->prox = NULL;

  return novo;
}

so_t *so_cria(contr_t *contr)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->contr = contr;
  self->paniquei = false;
  self->cpue = cpue_cria();

  self->proc_bloq = NULL;
  self->proc_prt = novo_proc(self, 0);
  self->proc_exec = self->proc_prt;

  //################################################################################
  //aqui escolhe entre um quantum grande e um pequeno
  //self->quantum = 25;      //quantum grande
  self->quantum = 15;       //quantum pequeno
  //################################################################################
  //aqui escolhe qual escalonador de processor será usado
  self->esc_ptr = &escalonador_round;       //escalonador circular
  strcpy(self->nome_esc_proc, "primeiro-pronto");
  //self->esc_ptr = &escalonador_curto;     //escalonador que escolhe o mais rápido
  //strcpy(self->nome_esc_proc, "mais-curto");
  //################################################################################
  //aqui escolhe qual escalonador de paginas será usado
  //self->esc_pag = &escalonador_fifo;        //escalonador first in first out
  //strcpy(self->nome_esc_pag, "firs-in-first-out");
  self->esc_pag = &escalonador_lru;       //escalonador least recently used
  strcpy(self->nome_esc_pag, "least-recentl-used");
  //################################################################################
  self->m_so = (metricas_so){0, 0, 0, 0, 0};

  self->n_quadros = MEM_TAM / PAG_TAM;
  self->quadros = calloc(self->n_quadros, sizeof(quadro_t));
  //debug 
  //t_printf("criados %d quadros", self->n_quadros);
  //coloca id nos quadros
  for (int i = 0; i < self->n_quadros; i++){
    self->quadros[i].id = i;
  }

  init_mem(self, self->proc_exec);
  mmu_usa_tab_pag(contr_mmu(self->contr), self->proc_exec->tab_pag);
  // coloca a CPU em modo usuário
  /*
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  cpue_muda_modo(self->cpue, usuario);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  */
  return self;
}

//################################################ FUNÇÕES DE DESTRUIÇÃO #######################################

void so_destroi(so_t *self)
{
  cpue_destroi(self->cpue);
  free(self->quadros);
  free(self);
}

//destrói um processo
void destroi_proc(proc_t* proc){
  cpue_destroi(proc->cpue_proc);
  mem_destroi(proc->mem_sec);
  tab_pag_destroi(proc->tab_pag);
  free(proc);
}

//################################################ TRATAMENTO DE SISOPS #######################################

// chamada de sistema para leitura de E/S
// recebe em A a identificação do dispositivo
// retorna em X o valor lido
//            A o código de erro
static void so_trata_sisop_le(so_t *self)
{
  int disp = cpue_A(self->cpue);
  int val;
  err_t err;

  if (es_pronto(contr_es(self->contr), disp, leitura)){
    //esta pronto
    err = es_le(contr_es(self->contr), disp, &val);
    cpue_muda_A(self->cpue, err);
    
    if (err == ERR_OK) {
      cpue_muda_X(self->cpue, val);
      // incrementa o PC
      cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
    }
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);

  } else {
    //nao esta pronto
    bloqueia_proc(self, (bloq_t)leitura, disp);
    cpue_muda_erro(self->cpue, ERR_OCUP, 0);
  }
}

// chamada de sistema para escrita de E/S
// recebe em A a identificação do dispositivo
//           X o valor a ser escrito
// retorna em A o código de erro
static void so_trata_sisop_escr(so_t *self)
{
  int disp = cpue_A(self->cpue);
  int val = cpue_X(self->cpue);
  err_t err;

  if (es_pronto(contr_es(self->contr), disp, escrita)){
    err = es_escreve(contr_es(self->contr), disp, val);
    cpue_muda_A(self->cpue, err);

    if (err == ERR_OK){
      cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
    }
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);

  } else {
    bloqueia_proc(self, (bloq_t)escrita, disp);
    cpue_muda_erro(self->cpue, ERR_OCUP, 0);
  }
}

// chamada de sistema para término do processo
static void so_trata_sisop_fim(so_t *self)
{
  //metricas
  self->proc_exec->m_proc.tempo_retorno = rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.existe_desde;
  salva_metricas_proc(self->proc_exec);

  proc_t* aux = self->proc_prt;
  if (self->proc_prt == self->proc_exec) { //o processo a ser destruido é o primeiro da lista de prontos
    self->proc_prt = self->proc_prt->prox;
    libera_quadros(self, self->proc_exec);
    destroi_proc(self->proc_exec);

  } else {
    while( (aux->prox != NULL) && (aux->prox->id_proc != self->proc_exec->id_proc) ){
      aux = aux->prox;
    } //agora o aux aponta pro anterior ao atual

    aux->prox = self->proc_exec->prox;
    libera_quadros(self, self->proc_exec);
    destroi_proc(self->proc_exec);
  }
  
  self->proc_exec = NULL;
  cpue_muda_erro(self->cpue, ERR_OK, 0);
}

// chamada de sistema para criação de processo
static void so_trata_sisop_cria(so_t *self)
{
  int id = cpue_A(self->cpue);
  proc_t* novo = novo_proc(self, id);

  //guarda o valor do rel no momento da criação
  //novo->m_proc.existe_desde = rel_agora(contr_rel(self->contr));

  proc_t* aux = self->proc_prt;
  while(aux->prox != NULL){
    aux = aux->prox;
  }
  aux->prox = novo;   //coloca o novo processo no fim da fila de prontos

  if (id == 1) {
    init_mem_p1(self, novo);
  } else
  if (id == 2) {
    init_mem_p2(self, novo);
  } else
  if (id == 3) {
    init_mem_a1(self, novo);
  } else {
    t_printf("Id inválido: %d", id);
    panico(self);
  }

  //troca a tabela de paginas a ser usada pela mmu de volta para a do processo criador
  mmu_usa_tab_pag(contr_mmu(self->contr), self->proc_exec->tab_pag);

  // incrementa o PC
  cpue_muda_PC(self->cpue, cpue_PC(self->cpue) + 2);
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// trata uma interrupção de chamada de sistema
static void so_trata_sisop(so_t *self)
{
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  // o tipo de chamada está no "complemento" do cpue
  so_chamada_t chamada = cpue_complemento(self->cpue);
  switch (chamada) {
    case SO_LE:
      so_trata_sisop_le(self);
      break;
    case SO_ESCR:
      so_trata_sisop_escr(self);
      break;
    case SO_FIM:
      so_trata_sisop_fim(self);
      break;
    case SO_CRIA:
      so_trata_sisop_cria(self);
      break;
    default:
      t_printf("so: chamada de sistema não reconhecida %d\n", chamada);
      panico(self);
  }
}

// trata uma interrupção de tempo do relógio
static void so_trata_tic(so_t *self)
{
  //debug
  //t_printf("Interrupção de relógio");
  if ((cpue_modo(self->cpue) == zumbi) || (self->proc_exec == NULL)){   //se a cpu estiver no mode zumbi, verifica os processos e tenta troca
    verif_processos(self);
    troca_processo(self);
    return;
  }
  //não está no modo zumbi, há um processo executando
  self->proc_exec->tempo_exec += rel_periodo(contr_rel(self->contr));   //aumenta o tempo que ficou executando
  verif_processos(self);                                                //desbloqueia o que puder de processos

  if (self->proc_exec->prox == NULL){                                //só há um processo pronto
    return;
  } else if (self->proc_exec->tempo_exec >= self->quantum){         //se extrapolou o quantum
    //metricas
    self->proc_exec->m_proc.n_preemp++;
    
    //preempção
    //gambiarra aqui, bloqueia o processo mas esse é imediatamente desbloquado na primeira verificação
    exec_copia_estado(contr_exec(self->contr), self->cpue);         //salva o contexto do proc
    cpue_copia(self->cpue, self->proc_exec->cpue_proc);
    bloqueia_proc(self, rel, -1);                                   
    troca_processo(self);                                           //faz a troca
    return;
  }
}

//trata uma falha de paginação
void so_trata_fal_pag(so_t* self)
{
  //debug
  //t_printf("falha de paginação");
  //metricas
  self->m_so.n_fal_pag++;

  //o endereço que causou falha de páginas
  int end = mmu_ultimo_endereco(contr_mmu(self->contr));
  carrega_pag_proc(self, end);

  exec_copia_estado(contr_exec(self->contr), self->cpue);
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  return;
}

// houve uma interrupção
void so_int(so_t *self, err_t err)
{
  //metricas
  self->m_so.n_int++;
  switch (err) {
    case ERR_SISOP:
      so_trata_sisop(self);     // ERR_OK se ta tudo bem, ERR_OCUP se deu ruim
      break;
    case ERR_TIC:
      so_trata_tic(self);
      return;
    case ERR_FALPAG:
      so_trata_fal_pag(self);
      return;
    default:
      t_printf("SO: interrupção não tratada [%s]", err_nome(err));
      self->paniquei = true;
  }

  if (cpue_erro(self->cpue) == ERR_OCUP){   //processo em exec bloqueou
    exec_copia_estado(contr_exec(self->contr), self->cpue);
    if (self->proc_exec != NULL){
      cpue_copia(self->cpue, self->proc_exec->cpue_proc);
    }
    verif_processos(self);
    troca_processo(self); 
  }
  if (self->proc_exec == NULL) { //processo em exec acabou
    verif_processos(self);
    troca_processo(self);
  }
}

//################################################ FUNÇÕES DE CONTROLE #######################################
// retorna false se o sistema deve ser desligado
bool so_ok(so_t *self)
{
  return !self->paniquei;
}

static void panico(so_t *self) 
{
  t_printf("Problema irrecuperável no SO");
  self->paniquei = true;
  salva_metricas_so(self);
}

//################################################ FUNÇÕES DE INICIALIZAÇÃO DE MEM #######################################

static void init_mem_p1(so_t* self, proc_t* proc){
  int p1[] = {
    #include "p1.maq"
  };
  int tam_p1 = sizeof(p1) / sizeof(p1[0]);

  inicializa_tab_pag(self, proc, tam_p1);

  //escreve na "memoria secundaria"
  err_t err;
  mem_t* mem = proc->mem_sec;
  for (int i = 0; i < tam_p1; i++) {
    err = mem_escreve(mem, i, p1[i]);
    if ( err != ERR_OK) {
      t_printf("so.mem_p1: erro de memória, endereco %d, erro %s\n", i, err_nome(err));
      panico(self);
    }
  }

}

static void init_mem_p2(so_t* self, proc_t* proc){
  int p2[] = {
    #include "p2.maq"
  };
  int tam_p2 = sizeof(p2) / sizeof(p2[0]);

  inicializa_tab_pag(self, proc, tam_p2);

  //escreve na "memoria secundaria"
  err_t err;
  mem_t* mem = proc->mem_sec;
  for (int i = 0; i < tam_p2; i++) {
    err = mem_escreve(mem, i, p2[i]);
    if ( err != ERR_OK) {
      t_printf("so.mem_p2: erro de memória, endereco %d, erro %s\n", i, err_nome(err));
      panico(self);
    }
  }
}

static void init_mem_a1(so_t* self, proc_t* proc){
  int a1[] = {
    #include "a1.maq"
  };
  int tam_a1 = sizeof(a1) / sizeof(a1[0]);

  inicializa_tab_pag(self, proc, tam_a1);

  //escreve na "memoria secundaria"
  err_t err;
  mem_t* mem = proc->mem_sec;
  for (int i = 0; i < tam_a1; i++) {
    err = mem_escreve(mem, i, a1[i]);
    if ( err != ERR_OK) {
      t_printf("so.mem_a1: erro de memória, endereco %d, erro %s\n", i, err_nome(err));
      panico(self);
    }
  }
}

// carrega um programa na memória
static void init_mem(so_t *self, proc_t* proc)
{
  // programa para executar na nossa CPU
  //debug
  //t_printf("init_mem");
  int init[] = {
    #include "init.maq"
  };
  int tam_init = sizeof(init)/sizeof(init[0]);

  inicializa_tab_pag(self, proc, tam_init);

  //escreve na "memoria secundaria"
  err_t err;
  mem_t* mem = proc->mem_sec;
  for (int i = 0; i < tam_init; i++) {
    err = mem_escreve(mem, i, init[i]);
    if ( err != ERR_OK) {
      t_printf("so.init_mem: erro de memória, endereco %d, erro %s\n", i, err_nome(err));
      panico(self);
    }
  }
}

//################################################ FUNÇÕES EXTRAS #######################################

static void salva_metricas_proc(proc_t* proc){
  FILE* file;

  if (proc->id_proc == 0){    //esse é o processo 0, reinicia o arquivo
    file = fopen("metricas.txt", "w");
  } else {                    //não é o processo 0, abre o arquivo no modo 'append'
    file = fopen("metricas.txt", "a");
  }

  if (file == NULL){
    t_printf("Não foi possível salvar dados do processo.");
    return;
  }
  char s[7][32] = {
    "Métricas do processo: ",
    "Tempo de retorno:     ",
    "Tempo bloqueado:      ",
    "Tempo em execução:    ",
    "Tempo esperando:      ",
    "Número de bloqueios:  ",
    "Número de preempções: ",
  };

  int d[7] = {
    proc->id_proc, 
    proc->m_proc.tempo_retorno, 
    proc->m_proc.tempo_bloq, 
    proc->m_proc.tempo_exec, 
    proc->m_proc.tempo_pronto, 
    proc->m_proc.n_bloqs, 
    proc->m_proc.n_preemp
  };

  fprintf(file, "%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n\n",
          s[0], d[0], 
          s[1], d[1],
          s[2], d[2],
          s[3], d[3],
          s[4], d[4],
          s[5], d[5],
          s[6], d[6]);
  fclose(file);
}

static void salva_metricas_so(so_t* so){
  FILE* file = fopen("metricas.txt", "a");

  if (file == NULL){
    t_printf("Não foi possível salvar dados do SO.");
    return;
  }
  char s[10][38] = {
    "Métricas do Sistema Operacional",
    "Escalonador de paginas:   ",
    "Tamanho da memória:       ",
    "Tamanho da página:        ",
    "Quantum:                  ",
    "Escalonador de processos: ",
    "Tempo total de sistema:   ",
    "Tempo total em execução:  ",
    "Número de interrupções:   ",
    "Número de falhas de pag:  "
  };

  int d[5] = {
    so->quantum,
    so->m_so.tempo_total, 
    so->m_so.tempo_total_exec,
    so->m_so.n_int,
    so->m_so.n_fal_pag,
  };

  fprintf(file, "%s\n%s %s\n%s %d\n%s %d\n%s %d\n%s %s\n%s %d\n%s %d\n%s %d\n%s %d\n\n", s[0],
          s[1], (so->nome_esc_pag),
          s[2], MEM_TAM,
          s[3], PAG_TAM,
          s[4], d[0],
          s[5], (so->nome_esc_proc),
          s[6], d[1],
          s[7], d[2],
          s[8], d[3],
          s[9], d[4]);
  fclose(file);
}
