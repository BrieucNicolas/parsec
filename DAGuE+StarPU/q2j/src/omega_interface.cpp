/*
 * Copyright (c) 2009-2010 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */


#include "dague_config.h"
#include "node_struct.h"
#include "utility.h"
#include "q2j.y.h"
#include "omega_interface.h"
#include "omega.h"
#include "starpu_struct.h"
#include <assert.h>
#include <map>
#include <set>
#include <list>
#include <sstream>
#include <inttypes.h>
#include <stdio.h>

//#define Q2J_ASSERT(_X_) do{ if(!(_X_)){ abort(); } }while(0)
#define Q2J_ASSERT(_X_) assert((_X_));

static map<string, string> q2j_colocated_map;
static set<node_t *> q2j_global_invariants;

struct _dep_t{
    node_t *src;
    node_t *dst;
    Relation *rel;
};

#define DEP_FLOW  0x1
#define DEP_OUT   0x2
#define DEP_ANTI  0x4

#define EDGE_INCOMING 0x0
#define EDGE_OUTGOING 0x1

#define LBOUND  0x0
#define UBOUND  0x1

#define SOURCE  0x0
#define SINK    0x1

extern int _q2j_produce_shmem_jdf;
extern int _q2j_verbose_warnings;

#if 0
extern void dump_und(und_t *und);
static void dump_full_und(und_t *und);
#endif

static void process_end_condition(node_t *node, F_And *&R_root, map<string, Variable_ID> ivars, node_t *lb, Relation &R);
static list<char *> print_edges_and_create_pseudotasks(set<dep_t *>outg_deps, set<dep_t *>incm_edges, Relation S, node_t *reference_data_element);
static char *create_pseudotask(task_t *parent_task, Relation S_es, Relation cond, node_t *data_element, char *var_pseudoname, int ptask_count, const char *inout);
static void print_pseudo_variables(set<dep_t *>out_deps, set<dep_t *>in_deps);
static Relation process_and_print_execution_space(node_t *node);
static inline set<expr_t *> find_all_EQs_with_var(const char *var_name, expr_t *exp);
static inline set<expr_t *> find_all_GEs_with_var(const char *var_name, expr_t *exp);
static set<expr_t *> find_all_constraints_with_var(const char *var_name, expr_t *exp, int constr_type);
static bool is_expr_simple(const expr_t *exp);
static const char *expr_tree_to_str(const expr_t *exp);
static string _expr_tree_to_str(const expr_t *exp);
static int expr_tree_contains_var(expr_t *root, const char *var_name);
static int expr_tree_contains_only_vars_in_set(expr_t *root, set<const char *>vars);
static void convert_if_condition_to_Omega_relation(node_t *node, F_And *R_root, map<string, Variable_ID> ivars, Relation &R);
static const char *find_bounds_of_var(expr_t *exp, const char *var_name, set<const char *> vars_in_bounds, Relation R);
static expr_t *solve_directly_solvable_EQ(expr_t *exp, const char *var_name, Relation R);
static void substitute_exp_for_var(expr_t *exp, const char *var_name, expr_t *root);
static map<string, Free_Var_Decl *> global_vars;
static list< pair<expr_t *, Relation> > simplify_conditions_and_split_disjunctions(Relation R, Relation S_es);
expr_t *relation_to_tree( Relation R );
static inline bool is_phony_Entry_task(task_t *task);
static inline bool is_phony_Exit_task(task_t *task);

#if 0
void dump_all_uses(und_t *def, var_t *head){
    int after_def = 0;
    var_t *var;
    und_t *und;

    for(var=head; NULL != var; var=var->next){
        for(und=var->und; NULL != und ; und=und->next){
            char *var_name = DA_var_name(DA_array_base(und->node));
            char *def_name = DA_var_name(DA_array_base(def->node));
            if( und == def ){
                after_def = 1;
            }
            if( is_und_read(und) && (0==strcmp(var_name, def_name)) ){
                printf("   ");
                if( after_def )
                    printf(" +%d ",und->task_num);
                else
                    printf(" -%d ",und->task_num);
                dump_full_und(und);
            }
        }
    }
}

void dump_full_und(und_t *und){
    node_t *tmp;

    printf("%d ",und->task_num);
    dump_und(und);
    printf(" ");
    for(tmp=und->node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        printf("%s:", DA_var_name(DA_loop_induction_variable(tmp)) );
        printf("{ %s, ", tree_to_str(DA_loop_lb(tmp)) );
        printf(" %s }", tree_to_str(DA_loop_ub(tmp)) );
        if( NULL != tmp->enclosing_loop )
            printf(",  ");
    }
    printf("\n");
}

void dump_UorD(node_t *node){
    node_t *tmp;
    list<char *> ind;
    printf("%s {",tree_to_str(node));


    for(tmp=node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        ind.push_front( DA_var_name(DA_loop_induction_variable(tmp)) );
    }
    for (list<char *>::iterator it=ind.begin(); it!=ind.end(); ++it){
        if( it != ind.begin() )
            printf(",");
        printf("%s",*it);
    }

    printf("}");
}
#endif

static inline bool is_phony_Entry_task(task_t *task){
    char *name = task->task_name;
    return (strstr(name, "DAGUE_IN_") == name);
}

static inline bool is_phony_Exit_task(task_t *task){
    char *name = task->task_name;
    return (strstr(name, "DAGUE_OUT_") == name);
}

void declare_globals_in_tree(node_t *node, set <char *> ind_names){
    char *var_name = NULL;

    switch( node->type ){
        case S_U_MEMBER:
            var_name = tree_to_str(node);
        case IDENTIFIER:
            if( NULL == var_name ){
                var_name = DA_var_name(node);
            }
            // If the var is one of the induction variables, do nothing
            set <char *>::iterator ind_it = ind_names.begin();
            for(; ind_it != ind_names.end(); ++ind_it){
                char *tmp_var = *ind_it;
                if( 0==strcmp(var_name, tmp_var) ){
                    return;
                }
            }
       
            // If the var is not already declared as a global symbol, declare it
            map<string, Free_Var_Decl *>::iterator it;
            it = global_vars.find(var_name);
            if( it == global_vars.end() ){
                global_vars[var_name] = new Free_Var_Decl(var_name);
            }

            return;
    }

    if( BLOCK == node->type ){
        node_t *tmp;
        for(tmp=node->u.block.first; NULL != tmp; tmp = tmp->next){
            declare_globals_in_tree(tmp, ind_names);
        }
    }else{
        int i;
        for(i=0; i<node->u.kids.kid_count; ++i){
            declare_globals_in_tree(node->u.kids.kids[i], ind_names);
        }
    }

    return;
}



// the parameter "R" (of type "Relation") needs to be passed by reference, otherwise a copy
// is passed every time the recursion goes deeper and the "handle" does not correspond to R.
void expr_to_Omega_coef(node_t *node, Constraint_Handle &handle, int sign, map<string, Variable_ID> vars, Relation &R){
    map<string, Variable_ID>::iterator v_it;
    map<string, Free_Var_Decl *>::iterator g_it;
    char *var_name=NULL;

    switch(node->type){
        case INTCONSTANT:
            handle.update_const(sign*DA_int_val(node));
            break;
        case S_U_MEMBER:
            var_name = tree_to_str(node);
        case IDENTIFIER:
            // look for the ID in the induction variables
            if( NULL==var_name ){
                var_name = DA_var_name(node);
            }
            v_it = vars.find(var_name);
            if( v_it != vars.end() ){
                Variable_ID v = v_it->second;
                handle.update_coef(v,sign);
                break;
            }
            // If not found yet, look for the ID in the global variables
            g_it = global_vars.find(var_name);
            if( g_it != global_vars.end() ){
                Variable_ID v = R.get_local(g_it->second);
                handle.update_coef(v,sign);
                break;
            }
            fprintf(stderr,"expr_to_Omega_coef(): Can't find \"%s\" in either induction, or global variables.\n", DA_var_name(node) );
            exit(-1);
        case ADD:
            expr_to_Omega_coef(node->u.kids.kids[0], handle, sign, vars, R);
            expr_to_Omega_coef(node->u.kids.kids[1], handle, sign, vars, R);
            break;
        case SUB:
            expr_to_Omega_coef(node->u.kids.kids[0], handle, sign, vars, R);
            expr_to_Omega_coef(node->u.kids.kids[1], handle, -sign, vars, R);
            break;
        default:
            fprintf(stderr,"expr_to_Omega_coef(): Can't turn type \"%d (%x)\" into Omega expression.\n",node->type, node->type);
            exit(-1);
    }
    return;
}


/*
 * Find the first loop that encloses both arguments
 */
node_t *find_closest_enclosing_loop(node_t *n1, node_t *n2){
    node_t *tmp1, *tmp2;

    for(tmp1=n1->enclosing_loop; NULL != tmp1; tmp1=tmp1->enclosing_loop ){
        for(tmp2=n2->enclosing_loop; NULL != tmp2; tmp2=tmp2->enclosing_loop ){
            if(tmp1 == tmp2)
                return tmp1;
        }
    }
    return NULL;
}

void add_array_subscript_equalities(Relation &R, F_And *R_root, map<string, Variable_ID> ivars, map<string, Variable_ID> ovars, node_t *def, node_t *use){
    int i,count;

    count = DA_array_dim_count(def);
    if( DA_array_dim_count(use) != count ){
        fprintf(stderr,"add_array_subscript_equalities(): ERROR: Arrays in USE and DEF do not have the same number of subscripts.");
        fprintf(stderr,"USE: %s\n",tree_to_str(use));
        fprintf(stderr,"DEF: %s\n",tree_to_str(def));
        Q2J_ASSERT(0);
    }

    for(i=0; i<count; i++){
        node_t *iv = DA_array_index(def, i);
        node_t *ov = DA_array_index(use, i);
        EQ_Handle hndl = R_root->add_EQ();
        expr_to_Omega_coef(iv, hndl, 1, ivars, R);
        expr_to_Omega_coef(ov, hndl, -1, ovars, R);
    }

    return;
}

////////////////////////////////////////////////////////////////////////////////
// This function assumes that the end condition of the for() loop conforms to the
// regular expression:
// k (<|<=) E0 ( && k (<|<=) Ei )*
// where "k" is the induction variable and "Ei" are expressions of induction
// variables, global variables and constants.
//
void process_end_condition(node_t *node, F_And *&R_root, map<string, Variable_ID> ivars, node_t *lb, Relation &R){
    Variable_ID ivar;
    GEQ_Handle imax;
    F_And *new_and;

    switch( node->type ){
        case L_AND:
            new_and = R_root->add_and();
            process_end_condition(node->u.kids.kids[0], new_and, ivars, lb, R);
            process_end_condition(node->u.kids.kids[1], new_and, ivars, lb, R);
            break;
// TODO: handle logical or (L_OR) as well.
//F_Or *or1 = R_root->add_or();
//F_And *and1 = or1->add_and();
        case LT:
            ivar = ivars[DA_var_name(DA_rel_lhs(node))];
            imax = R_root->add_GEQ();
            expr_to_Omega_coef(DA_rel_rhs(node), imax, 1, ivars, R);
            imax.update_coef(ivar,-1);
            imax.update_const(-1);
            if (lb != NULL ){ // Add the condition LB < UB
                GEQ_Handle lb_ub;
                lb_ub = R_root->add_GEQ();
                expr_to_Omega_coef(DA_rel_rhs(node), lb_ub, 1, ivars, R);
                expr_to_Omega_coef(lb, lb_ub, -1, ivars, R);
                lb_ub.update_const(-1);
            }
            break;
        case LE:
            ivar = ivars[DA_var_name(DA_rel_lhs(node))];
            imax = R_root->add_GEQ();
            expr_to_Omega_coef(DA_rel_rhs(node), imax, 1, ivars, R);
            imax.update_coef(ivar,-1);
            if (lb != NULL ){ // Add the condition LB < UB
                GEQ_Handle lb_ub;
                lb_ub = R_root->add_GEQ();
                expr_to_Omega_coef(DA_rel_rhs(node), lb_ub, 1, ivars, R);
                expr_to_Omega_coef(lb, lb_ub, -1, ivars, R);
            }
            break;
        default:
            fprintf(stderr,"ERROR: process_end_condition() cannot deal with node of type: %s in: \"%s\"\n", DA_type_name(node),tree_to_str(node) );
            exit(-1);
    }

}


void process_condition(node_t *node, F_And *&R_root, map<string, Variable_ID> ivars, Relation &R){
    Variable_ID ivar;
    GEQ_Handle cond;
    EQ_Handle econd;
    F_And *new_and;
    F_Or  *new_or;
    F_Not *neg;

    switch( node->type ){
        case L_AND:
            new_and = R_root->add_and();
            process_condition(node->u.kids.kids[0], new_and, ivars, R);
            process_condition(node->u.kids.kids[1], new_and, ivars, R);
            break;
        case L_OR:
            new_or = R_root->add_or();
            new_and = new_or->add_and();
            process_condition(node->u.kids.kids[0], new_and, ivars, R);
            process_condition(node->u.kids.kids[1], new_and, ivars, R);

            fprintf(stderr,"WARNING: Logical OR in conditions has not been thoroughly tested. Confirm correctness:\n");
            fprintf(stderr,"Cond: %s\n",tree_to_str(node));
            fprintf(stderr,"R: ");
            R.print_with_subs(stderr);

            break;
        case LT:
            cond = R_root->add_GEQ();
            expr_to_Omega_coef(DA_rel_rhs(node), cond, 1, ivars, R);
            expr_to_Omega_coef(DA_rel_lhs(node), cond, -1, ivars, R);
            cond.update_const(-1);
            break;
        case LE:
            cond = R_root->add_GEQ();
            expr_to_Omega_coef(DA_rel_rhs(node), cond, 1, ivars, R);
            expr_to_Omega_coef(DA_rel_lhs(node), cond, -1, ivars, R);
            break;
        case EQ_OP:
            econd = R_root->add_EQ();
            expr_to_Omega_coef(DA_rel_rhs(node), econd, 1, ivars, R);
            expr_to_Omega_coef(DA_rel_lhs(node), econd, -1, ivars, R);
        case NE_OP:
            neg = R_root->add_not();
            new_and = neg->add_and();
            econd = new_and->add_EQ();
            expr_to_Omega_coef(DA_rel_rhs(node), econd, 1, ivars, R);
            expr_to_Omega_coef(DA_rel_lhs(node), econd, -1, ivars, R);
        default:
            fprintf(stderr,"ERROR: process_condition() cannot deal with node of type: %s in: \"%s\"\n", DA_type_name(node),tree_to_str(node) );
            exit(-1);
    }

}

static void convert_if_condition_to_Omega_relation(node_t *node, F_And *R_root, map<string, Variable_ID> ivars, Relation &R){
    map<string, Free_Var_Decl *>::iterator g_it;
    char **known_vars;
    int count;

    count = 0;
    for(node_t *tmp=node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop )
        count++;
    for(g_it=global_vars.begin(); g_it != global_vars.end(); g_it++ )
        count++;

    known_vars = (char **)calloc(count+1, sizeof(char *));
    count = 0;

    // We consider the induction variables of all the loops enclosing the if() to be known
    for(node_t *tmp=node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        const char *var_name = DA_var_name(DA_loop_induction_variable(tmp));
        known_vars[count++] = strdup(var_name);
    }

    // We allso consider the global variables to be known.
    for(g_it=global_vars.begin(); g_it != global_vars.end(); g_it++ ){
        known_vars[count++] = strdup((*g_it).first.c_str());
    }

    // Check if the condition has variables we don't know how to deal with.
    if( !DA_tree_contains_only_known_vars(DA_if_condition(node), known_vars) ){
        fprintf(stderr,"WARNING: if statement: \"%s\" contains unknown variables, so it will be ignored\n",tree_to_str(DA_if_condition(node)));
        fprintf(stderr,"WARNING: known variables are listed below:\n");
        for(int i=0; i<count; i++)
            fprintf(stderr,"%s\n",known_vars[i]);
        exit(-1);
    }

    // Do some memory clean-up
    while(count){
        free(known_vars[--count]);
    }
    free(known_vars);

    // Create the actual condition in the Omega relation "R"
    process_condition(DA_if_condition(node), R_root, ivars, R);

    return;
}

Relation create_exit_relation(node_t *exit, node_t *def){
    int i, src_var_count, dst_var_count;
    node_t *tmp, *use;
    char **def_ind_names;
    map<string, Variable_ID> ivars;

    use = exit;

    src_var_count = def->loop_depth;
    dst_var_count = DA_array_dim_count(def);

    Relation R(src_var_count, dst_var_count);

    // Store the names of the induction variables of the loops that enclose the use
    // and make the last item NULL so we know where the array terminates.
    def_ind_names = (char **)calloc(src_var_count+1, sizeof(char *));
    def_ind_names[src_var_count] = NULL;
    i=src_var_count-1;
    for(tmp=def->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        def_ind_names[i] = DA_var_name(DA_loop_induction_variable(tmp));
        --i;
    }

    // Name the input variables using the induction variables of the loops that enclose the def.
    for(i=0; i<src_var_count; ++i){
        R.name_input_var(i+1, def_ind_names[i]);
        ivars[def_ind_names[i]] = R.input_var(i+1);
    }

    // Name the output variables using temporary names "Var_i"
    for(i=0; i<dst_var_count; ++i){
        stringstream ss;
        ss << "Var_" << i;

        R.name_output_var(i+1, ss.str().c_str() );
    }

    F_And *R_root = R.add_and();

    // Bound all induction variables of the loops enclosing the DEF
    // using the loop bounds
    for(tmp=def->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        // Form the Omega expression for the lower bound
        Variable_ID ivar = ivars[DA_var_name(DA_loop_induction_variable(tmp))];

        GEQ_Handle imin = R_root->add_GEQ();
        imin.update_coef(ivar,1);
        expr_to_Omega_coef(DA_loop_lb(tmp), imin, -1, ivars, R);

        // Form the Omega expression for the upper bound
        process_end_condition(DA_for_econd(tmp), R_root, ivars, NULL, R);
    }

    // Take into account all the conditions of all enclosing if() statements.
    for(tmp=def->enclosing_if; NULL != tmp; tmp=tmp->enclosing_if ){
        convert_if_condition_to_Omega_relation(tmp, R_root, ivars, R);
    }

    // Add equalities between corresponding input and output array indexes
    int count = DA_array_dim_count(def);
    for(i=0; i<count; i++){
        node_t *iv = DA_array_index(def, i);
        EQ_Handle hndl = R_root->add_EQ();

        hndl.update_coef(R.output_var(i+1), 1);
        expr_to_Omega_coef(iv, hndl, -1, ivars, R);
    }

    R.simplify();
    return R;
}

map<node_t *, Relation> create_entry_relations(node_t *entry, var_t *var, int dep_type){
    int i, src_var_count, dst_var_count;
    und_t *und;
    node_t *tmp, *def, *use;
    char **use_ind_names;
    map<string, Variable_ID> ivars, ovars;
    map<node_t *, Relation> dep_edges;

    def = entry;
    for(und=var->und; NULL != und ; und=und->next){

        if( ((DEP_FLOW==dep_type) && (!is_und_read(und))) || ((DEP_OUT==dep_type) && (!is_und_write(und))) ){
            continue;
        }

        use = und->node;

        dst_var_count = use->loop_depth;
        // we'll make the source match the destination, whatever that is
        src_var_count = DA_array_dim_count(use);

        Relation R(src_var_count, dst_var_count);

        // Store the names of the induction variables of the loops that enclose the use
        // and make the last item NULL so we know where the array terminates.
        use_ind_names = (char **)calloc(dst_var_count+1, sizeof(char *));
        use_ind_names[dst_var_count] = NULL;
        i=dst_var_count-1;
        for(tmp=use->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            use_ind_names[i] = DA_var_name(DA_loop_induction_variable(tmp));
            --i;
        }

        // Name the input variables using temporary names "Var_i"
        // The input variables will remain unnamed, but they should disappear because of the equalities
        for(i=0; i<src_var_count; ++i){
            stringstream ss;
            ss << "Var_" << i;

            R.name_input_var(i+1, ss.str().c_str() );
        }

        // Name the output variables using the induction variables of the loops that enclose the use.
        for(i=0; i<dst_var_count; ++i){
            R.name_output_var(i+1, use_ind_names[i]);
            ovars[use_ind_names[i]] = R.output_var(i+1);
        }

        F_And *R_root = R.add_and();

        // Bound all induction variables of the loops enclosing the USE
        // using the loop bounds
        for(tmp=use->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            // Form the Omega expression for the lower bound
            Variable_ID ovar = ovars[DA_var_name(DA_loop_induction_variable(tmp))];

            GEQ_Handle imin = R_root->add_GEQ();
            imin.update_coef(ovar,1);
            expr_to_Omega_coef(DA_loop_lb(tmp), imin, -1, ovars, R);

            // Form the Omega expression for the upper bound
            process_end_condition(DA_for_econd(tmp), R_root, ovars, NULL, R);
        }

        // Take into account all the conditions of all enclosing if() statements.
        for(tmp=use->enclosing_if; NULL != tmp; tmp=tmp->enclosing_if ){
            convert_if_condition_to_Omega_relation(tmp, R_root, ovars, R);
        }

        // Add equalities between corresponding input and output array indexes
        int count = DA_array_dim_count(use);
        for(i=0; i<count; i++){
            node_t *ov = DA_array_index(use, i);
            EQ_Handle hndl = R_root->add_EQ();

            hndl.update_coef(R.input_var(i+1), 1);
            expr_to_Omega_coef(ov, hndl, -1, ovars, R);
        }

        R.simplify();
        if( R.is_upper_bound_satisfiable() || R.is_lower_bound_satisfiable() ){
            dep_edges[use] = R;
        }

    }

    return dep_edges;
}




////////////////////////////////////////////////////////////////////////////////
//
// The variable names and comments in this function abuse the terms "def" and "use".
// It would be more acurate to use the terms "source" and "destination" for the edges.
map<node_t *, Relation> create_dep_relations(und_t *def_und, var_t *var, int dep_type, node_t *exit_node){
    int i, after_def = 0;
    int src_var_count, dst_var_count;
    und_t *und;
    node_t *tmp, *def, *use;
    char **def_ind_names;
    char **use_ind_names;
    map<string, Variable_ID> ivars, ovars;
    map<node_t *, Relation> dep_edges;

    // In the case of anti-dependencies (write after read) "def" is really a USE.
    def = def_und->node;
    src_var_count = def->loop_depth;
    after_def = 0;
    for(und=var->und; NULL != und ; und=und->next){
        char *var_name, *def_name;
 
        // skip anti-dependencies that go to the phony output task.
        if( DEP_ANTI == dep_type && is_phony_Exit_task(und->node->task) ){
            continue;
        }
   
        var_name = DA_var_name(DA_array_base(und->node));
        def_name = DA_var_name(DA_array_base(def));
        Q2J_ASSERT( !strcmp(var_name, def_name) );

        // Make sure that it's a proper flow (w->r) or anti (r->w) or output (w->w) dependency
        if( ((DEP_FLOW==dep_type) && (!is_und_read(und))) || ((DEP_OUT==dep_type) && (!is_und_write(und))) || ((DEP_ANTI==dep_type) && (!is_und_write(und))) ){
            // Since we'll bail, let's first check if this is the definition.
            if( und == def_und ){
                after_def = 1;
            }
            continue;
        }

        // If the types of source and destination are not overlapping, then ignore this edge.
        if(def_und->type && und->type && !(def_und->type & und->type)){
            continue;
        }

        // In the case of output dependencies (write after write) "use" is really a DEF.
        use = und->node;

        dst_var_count = use->loop_depth;
        Relation R(src_var_count, dst_var_count);

        // Store the names of the induction variables of the loops that enclose the definition
        // and make the last item NULL so we know where the array terminates.
        def_ind_names = (char **)calloc(src_var_count+1, sizeof(char *));
        def_ind_names[src_var_count] = NULL;
        i=src_var_count-1;
        for(tmp=def->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            def_ind_names[i] = DA_var_name(DA_loop_induction_variable(tmp));
            --i;
        }

        // Store the names of the induction variables of the loops that enclose the use
        // and make the last item NULL so we know where the array terminates.
        use_ind_names = (char **)calloc(dst_var_count+1, sizeof(char *));
        use_ind_names[dst_var_count] = NULL;
        i=dst_var_count-1;
        for(tmp=use->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            use_ind_names[i] = DA_var_name(DA_loop_induction_variable(tmp));
            --i;
        }

        // Name the input variables using the induction variables of the loops
        // that enclose the definition
        for(i=0; i<src_var_count; ++i){
            R.name_input_var(i+1, def_ind_names[i]);
             // put it in a map so we can find it later
            ivars[def_ind_names[i]] = R.input_var(i+1);
        }

        // Name the output variables using the induction variables of the loops
        // that enclose the use
        for(i=0; i<dst_var_count; ++i){
            R.name_output_var(i+1, use_ind_names[i]);
            // put it in a map so we can find it later
            ovars[use_ind_names[i]] = R.output_var(i+1);
        }

        F_And *R_root = R.add_and();

        // Bound all induction variables of the loops enclosing the DEF
        // using the loop bounds
        for(tmp=def->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            // Form the Omega expression for the lower bound
            Variable_ID ivar = ivars[DA_var_name(DA_loop_induction_variable(tmp))];

            GEQ_Handle imin = R_root->add_GEQ();
            imin.update_coef(ivar,1);
            expr_to_Omega_coef(DA_loop_lb(tmp), imin, -1, ivars, R);

            // Form the Omega expression for the upper bound
            process_end_condition(DA_for_econd(tmp), R_root, ivars, NULL, R);
        }

        // Take into account all the conditions of all if() statements enclosing the DEF.
        for(tmp=def->enclosing_if; NULL != tmp; tmp=tmp->enclosing_if ){
            convert_if_condition_to_Omega_relation(tmp, R_root, ivars, R);
        }

        // Bound all induction variables of the loops enclosing the USE
        // using the loop bounds
        for(tmp=use->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
            // Form the Omega expression for the lower bound
            Variable_ID ovar = ovars[DA_var_name(DA_loop_induction_variable(tmp))];

            GEQ_Handle imin = R_root->add_GEQ();
            imin.update_coef(ovar,1);
            expr_to_Omega_coef(DA_loop_lb(tmp), imin, -1, ovars, R);

            // Form the Omega expression for the upper bound
            process_end_condition(DA_for_econd(tmp), R_root, ovars, NULL, R);
        }

        // Take into account all the conditions of all if() statements enclosing the USE.
        for(tmp=use->enclosing_if; NULL != tmp; tmp=tmp->enclosing_if ){
            convert_if_condition_to_Omega_relation(tmp, R_root, ovars, R);
        }

        // Add inequalities of the form (m'>=m || n'>=n || ...) or the form (m'>m || n'>n || ...) if the DU chain is
        // "normal", or loop carried, respectively. The outermost enclosing loop HAS to be k'>=k, it is not
        // part of the "or" conditions.  In the loop carried deps, the outer most loop is ALSO in the "or" conditions,
        // so we have (m'>m || n'>n || k'>k) && k'>=k
        // In addition, if a flow edge is going from a task to itself (because the def and use seemed to be in different
        // lines) it also needs to have greater-than instead of greater-or-equal relationships for the induction variables.

        node_t *encl_loop = find_closest_enclosing_loop(use, def);

        // If USE and DEF are in the same task and that is not in a loop, there is no data flow, go to the next USE.
        if( (NULL == encl_loop) && (def->task == use->task) && ((DEP_ANTI!=dep_type) || (def != use)) ){
            continue;
        }
        
        if( NULL == encl_loop && 
            !( after_def && (def->task != use->task) ) &&
            !( (DEP_ANTI==dep_type) && (after_def||(def==use)) ) ){

            if( _q2j_verbose_warnings ){
                fprintf(stderr,"WARNING: In create_dep_relations() ");
                fprintf(stderr,"Destination is before Source and they do not have a common enclosing loop:\n");
                fprintf(stderr,"WARNING: Destination:%s %s\n", tree_to_str(use), use->task->task_name );
                fprintf(stderr,"WARNING: Source:%s %s\n", tree_to_str(def), def->task->task_name);
            }
            continue;
        }

        // Create a conjunction to enforce the iteration vectors of src and dst to be Is<=Id or Is<Id.
        if( NULL != encl_loop ){

            F_Or *or1 = R_root->add_or();
            for(tmp=encl_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
                F_And *and1 = or1->add_and();

                char *var_name = DA_var_name(DA_loop_induction_variable(tmp));
                Variable_ID ovar = ovars[var_name];
                Variable_ID ivar = ivars[var_name];
                GEQ_Handle ge = and1->add_GEQ();
                ge.update_coef(ovar,1);
                ge.update_coef(ivar,-1);
                // Create Relation due to "normal" DU chain.
                if( (after_def && (def->task != use->task)) && (tmp == encl_loop) ){
                    // If there this is "normal" DU chain, i.e. there is direct path from src to dst, then the inner most common loop can be n<=n'
                    ;
                }else{
                    // If this can only be a loop carried dependency, then the iteration vectors have to be strictly Isrc < Idst
                    ge.update_const(-1);
                }

                for(node_t *tmp2=tmp->enclosing_loop; NULL != tmp2; tmp2=tmp2->enclosing_loop ){
                    char *var_name = DA_var_name(DA_loop_induction_variable(tmp2));
                    Variable_ID ovar = ovars[var_name];
                    Variable_ID ivar = ivars[var_name];
                    EQ_Handle eq = and1->add_EQ();
                    eq.update_coef(ovar,1);
                    eq.update_coef(ivar,-1);
                }
            }
        }

        // Add equalities demanded by the array subscripts. For example is the DEF is A[k][k] and the
        // USE is A[m][n] then add (k=m && k=n).
        add_array_subscript_equalities(R, R_root, ivars, ovars, def, use);

        R.simplify();
        if( R.is_upper_bound_satisfiable() || R.is_lower_bound_satisfiable() ){
            dep_edges[use] = R;
        }

        // if this is the definition, all the uses that will follow will be "normal" DU chains.
        if( und == def_und ){
            after_def = 1;
        }
    }

    if( DEP_FLOW==dep_type ){
        dep_edges[exit_node] = create_exit_relation(exit_node, def);
    }

    return dep_edges;
}


void declare_global_vars(node_t *node){
    map<string, Free_Var_Decl *> tmp_map;
    node_t *tmp;


    if( FOR == node->type ){
        set <char *> ind_names;

        // Store the names of the induction variables of all the loops that enclose this loop
        for(tmp=node; NULL != tmp; tmp=tmp->enclosing_loop ){
            ind_names.insert( DA_var_name(DA_loop_induction_variable(tmp)) );
        }

        // Find all the variables in the lower bound that are not induction variables and
        // declare them as global variables (symbolic)
        declare_globals_in_tree(DA_loop_lb(node), ind_names);

        // Find all the variables in the upper bound that are not induction variables and
        // declare them as global variables (symbolic)
        declare_globals_in_tree(DA_for_econd(node), ind_names);
    }


    if( BLOCK == node->type ){
        for(tmp=node->u.block.first; NULL != tmp; tmp = tmp->next){
            declare_global_vars(tmp);
        }
    }else{
        int i;
        for(i=0; i<node->u.kids.kid_count; ++i){
            declare_global_vars(node->u.kids.kids[i]);
        }
    }

    return;
}

long int getVarCoeff(expr_t *root, const char * var_name){
    switch( root->type ){
        case MUL:
            if( INTCONSTANT == root->l->type ){
                Q2J_ASSERT( (IDENTIFIER == root->r->type) && !strcmp(var_name, root->r->value.name) );
                return root->l->value.int_const;
            }else if( INTCONSTANT == root->r->type ){
                Q2J_ASSERT( (IDENTIFIER == root->l->type) && !strcmp(var_name, root->l->value.name) );
                return root->r->value.int_const;
            }else{
                fprintf(stderr,"ERROR: getVarCoeff(): malformed expression: \"%s\"\n",expr_tree_to_str(root));
                exit(-1);
            }
            break; // although control can never reach this point

        default:
            if( expr_tree_contains_var(root->l, var_name) ){
                return getVarCoeff(root->l, var_name);
            }else if( expr_tree_contains_var(root->r, var_name) ){
                return getVarCoeff(root->r, var_name);
            }else{
                fprintf(stderr,"ERROR: getVarCoeff(): tree: \"%s\" does not contain variable: \"%s\"\n",expr_tree_to_str(root), var_name);
                exit(-1);
            }
            break; // although control can never reach this point
    }

    // control should never reach this point
    Q2J_ASSERT(0);
    return 0;
}


void free_tree( expr_t *root ){
//FIXME: do something
    return;
}

// WARNING: this function it destructive.  It actually removes nodes from the tree and deletes them altogether.
// In many cases you will need to pass a copy of the tree to this function.
expr_t *removeVarFromTree(expr_t *root, const char *var_name){

    // If we are given a MUL directly it means the constraint is of the form a*X = b and
    // we were given the "a*X" part.  In that case, we return NULL and the caller will
    // know what to do.
    if( MUL == root->type ){
        if( expr_tree_contains_var(root->l, var_name) || expr_tree_contains_var(root->r, var_name) ){
            return NULL;
        }
    }

    if( expr_tree_contains_var(root->l, var_name) ){
        if( MUL == root->l->type ){
            free_tree( root->l );
            return root->r;
        }else{
            root->l = removeVarFromTree(root->l, var_name);
            return root;
        }
    }else if( expr_tree_contains_var(root->r, var_name) ){
        if( MUL == root->r->type ){
            free_tree( root->r );
            return root->l;
        }else{
            root->r = removeVarFromTree(root->r, var_name);
            return root;
        }
    }else{
        fprintf(stderr,"ERROR: removeVarFromTree(): tree: \"%s\" does not contain variable: \"%s\"\n",expr_tree_to_str(root), var_name);
        exit(-1);
    }

}

expr_t *copy_tree(expr_t *root){
    expr_t *e;

    if( NULL == root )
        return NULL;

    e = (expr_t *)calloc( 1, sizeof(expr_t) );
    e->type = root->type;
    if( INTCONSTANT == root->type )
        e->value.int_const = root->value.int_const;
    else if( IDENTIFIER == root->type )
        e->value.name = strdup(root->value.name);
    e->l = copy_tree(root->l);
    e->r = copy_tree(root->r);

    return e;
}


// WARNING: This function is destructive in that it actually changes
// the nodes of the tree.  If you need your original tree intact,
// you should pass it a copy of the tree.
void flipSignOfTree(expr_t *root){
    if( NULL == root )
        return;

    switch( root->type){
        case INTCONSTANT:
            root->value.int_const *= -1;
            break;

        case MUL:
            if( INTCONSTANT == root->l->type ){
                root->l->value.int_const *= -1;
            }else if( INTCONSTANT == root->r->type ){
                root->r->value.int_const *= -1;
            }else{
                fprintf(stderr,"ERROR: flipSignOfTree(): malformed expression: \"%s\"\n",expr_tree_to_str(root));
            }
            break;

        default:
            flipSignOfTree(root->l);
            flipSignOfTree(root->r);
    }

    return;
}


// Move the first argument "e_src" to the second argument "e_dst".
// That means that we flip the sign of each element of e_src and add it to e_dst.
//
// WARNING: This function is destructive in that it actually changes
// the nodes of the tree e_src.  If you need your original tree intact,
// you should pass it a copy of the tree.
expr_t *moveToOtherSideOfEQ(expr_t *e_src, expr_t *e_dst){
    expr_t *e;

    if( (INTCONSTANT == e_src->type) && (0 == e_src->value.int_const) )
        return e_dst;

    e = (expr_t *)calloc( 1, sizeof(expr_t) );
    e->type = ADD;
    e->l = e_dst;
    flipSignOfTree(e_src);
    e->r = e_src;

    return e;
}

expr_t *solveConstraintForVar(expr_t *constr_exp, const char *var_name){
    expr_t *e, *e_other;
    long int c;

    Q2J_ASSERT( (EQ_OP == constr_exp->type) || (GE == constr_exp->type) );

    if( expr_tree_contains_var(constr_exp->l, var_name) ){
        e = copy_tree(constr_exp->l);
        e_other = copy_tree(constr_exp->r);
    }else if( expr_tree_contains_var(constr_exp->r, var_name) ){
        e = copy_tree(constr_exp->r);
        e_other = copy_tree(constr_exp->l);
    }else{
        Q2J_ASSERT(0);
    }

    c = getVarCoeff(e, var_name);
    Q2J_ASSERT( 0 != c );
    e = removeVarFromTree(e, var_name);

    if( NULL == e ){
        // We are here because the constraint is of the form a*X = b and removeVarFromTree() has
        // returned NULL. Therefore we only keep the RHS, since the coefficient has been saved earlier.
        e = e_other;
    }else{
        // Else removeVarFromTree() returned an expression with the variable removed and we need
        // to move it to the other side.
        if( c < 0 )
            e = moveToOtherSideOfEQ(e_other, e);
        else
            e = moveToOtherSideOfEQ(e, e_other);
    }

    c = labs(c);
    if( c != 1 ){
        fprintf(stderr,"WARNING: solveConstraintForVar() resulted in the generation of a DIV. This has not been tested thoroughly\n");
        expr_t *e_cns = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_cns->type = INTCONSTANT;
        e_cns->value.int_const = c;

        expr_t *e_div = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_div->type = DIV;
        e_div->l = e;
        e_div->r = e_cns;
        e = e_div;
    }
    return e;

}


/*
 * This function returns 1 if the tree passes as first parameter contains
 * only variables in the set passed as second parameter, or no variables at all.
 */
static int expr_tree_contains_only_vars_in_set(expr_t *root, set<const char *>vars){
    set<const char *>::iterator it;

    if( NULL == root )
        return 1;

    switch( root->type ){
        case IDENTIFIER:
            for ( it=vars.begin() ; it != vars.end(); it++ ){
                if( !strcmp(*it, root->value.name) )
                    return 1;
            }
            return 0;
        default:
            if( !expr_tree_contains_only_vars_in_set(root->l, vars) )
                return 0;
            if( !expr_tree_contains_only_vars_in_set(root->r, vars) )
                return 0;
            return 1;
    }
    return 1;
}


/*
 * This function returns 1 if the tree passed as first parameter contains the
 * variable passed as second parameter anywhere in the tree.
 */
int expr_tree_contains_var(expr_t *root, const char *var_name){

    if( NULL == root )
        return 0;

    switch( root->type ){
        case IDENTIFIER:
            if( !strcmp(var_name, root->value.name) )
                return 1;
            return 0;
        default:
            if( expr_tree_contains_var(root->l, var_name) )
                return 1;
            if( expr_tree_contains_var(root->r, var_name) )
                return 1;
            return 0;
    }
    return 0;
}

static const char *find_bounds_of_var(expr_t *exp, const char *var_name, set<const char *> vars_in_bounds, Relation R){
    char *lb = NULL, *ub = NULL;
    stringstream ss;
    set<expr_t *> ges = find_all_GEs_with_var(var_name, exp);
    bool is_lb_simple = false, is_ub_simple = false;
    bool is_lb_C = false, is_ub_C = false;

    map<string, Free_Var_Decl *>::iterator g_it;
    for(g_it=global_vars.begin(); g_it != global_vars.end(); g_it++ ){
        vars_in_bounds.insert( strdup((*g_it).first.c_str()) );
    }

    set<expr_t *>::iterator e_it;
    for(e_it=ges.begin(); e_it!=ges.end(); e_it++){
        int exp_has_output_vars = 0;

        expr_t *ge_exp = *e_it;
        int c = getVarCoeff(ge_exp, var_name);
        Q2J_ASSERT(c);

        expr_t *rslt_exp = solveConstraintForVar(ge_exp, var_name);

        if( !R.is_set() ){
            for(int i=0; i<R.n_out(); i++){
                const char *ovar = R.output_var(i+1)->char_name();
                if( expr_tree_contains_var(rslt_exp, ovar) ){
                    exp_has_output_vars = 1;
                    break;
                }
            }
        }

        // If the expression has output variables, or variables we don't want it to contain, we need to ignore it
        if( exp_has_output_vars || !expr_tree_contains_only_vars_in_set(rslt_exp, vars_in_bounds) )
            continue;

        if( c > 0 ){ // then lower bound
            if( NULL == lb ){
                is_lb_simple = is_expr_simple(rslt_exp);
                lb = strdup( expr_tree_to_str(rslt_exp) );
            }else{
                is_lb_simple = true;
                is_lb_C = true;
                asprintf(&lb, "MAX((%s),(%s))",strdup(lb),expr_tree_to_str(rslt_exp));
            }
        }else{ // else upper bound
            if( NULL == ub ){
                is_ub_simple = is_expr_simple(rslt_exp);
                ub = strdup( expr_tree_to_str(rslt_exp) );
            }else{
                is_ub_simple = true;
                is_ub_C = true;
                asprintf(&ub, "MIN((%s),(%s))",strdup(ub),expr_tree_to_str(rslt_exp));
            }
        }

    }

    if( is_lb_C ){
        asprintf(&lb, "inline_c %%{ return %s; %%}",lb);
    }
    if( is_ub_C ){
        asprintf(&ub, "inline_c %%{ return %s; %%}",ub);
    }

    if( NULL != lb ){
        if( is_lb_simple ){
            ss << lb;
        }else{
            ss << "(" << lb << ")";
        }
        free(lb);
    }else{
        ss << "??";
    }


    ss << "..";

    if( NULL != ub ){
        if( is_ub_simple ){
            ss << ub;
        }else{
            ss << "(" << ub << ")";
        }
        free(ub);
    }else{
        ss << "??";
    }

    return strdup(ss.str().c_str());
}

////////////////////////////////////////////////////////////////////////////////
//
expr_t *removeNodeFromAND(expr_t *node, expr_t *root){
    if( NULL == root )
        return NULL;

    if( node == root->l ){
        Q2J_ASSERT( L_AND == root->type );
        return root->r;
    }

    if( node == root->r ){
        Q2J_ASSERT( L_AND == root->type );
        return root->l;
    }

    root->r = removeNodeFromAND(node, root->r);
    root->l = removeNodeFromAND(node, root->l);
    return root;

}

expr_t *createGEZero(expr_t *exp){
    expr_t *zero = (expr_t *)calloc( 1, sizeof(expr_t) );
    zero->type = INTCONSTANT;
    zero->value.int_const = 0;

    expr_t *e = (expr_t *)calloc( 1, sizeof(expr_t) );
    e->type = GE;
    e->l = exp;
    e->r = zero;

    return e;
}

////////////////////////////////////////////////////////////////////////////////
//
// This function uses the transitivity of the ">=" operator to eliminate variables from
// the GEs.  As an example, if we are trying to eliminate X from: X-a>=0 && b-X-1>=0 we
// solve for "X" and get: b-1>=X && X>=a which due to transitivity gives us: b-1>=a
expr_t *eliminateVarUsingTransitivity(expr_t *exp, const char *var_name, Relation R){
    set<expr_t *> ges;
    set<expr_t *> ubs, lbs;
    set<expr_t *>::iterator it;

    ges = find_all_GEs_with_var(var_name, exp);

    // solve all GEs that involve "var_name" and get all the lower/upper bounds.
    for(it=ges.begin(); it!=ges.end(); it++){
        expr_t *ge_exp = *it;
        int c = getVarCoeff(ge_exp, var_name);
        Q2J_ASSERT(c);

        expr_t *rslt_exp = solveConstraintForVar(ge_exp, var_name);

        if( c > 0 ){ // then lower bound
            lbs.insert( copy_tree(rslt_exp) );
        }else{ // else upper bound
            ubs.insert( copy_tree(rslt_exp) );
        }

        // Remove each GE that involves "var_name" from "exp"
        exp = removeNodeFromAND(ge_exp, exp);
    }

    // Add all the combinations of LBs and UBs in the expression
    for(it=ubs.begin(); it!=ubs.end(); it++){
        set<expr_t *>::iterator lb_it;
        for(lb_it=lbs.begin(); lb_it!=lbs.end(); lb_it++){
            expr_t *new_e = moveToOtherSideOfEQ(*lb_it, *it);
            new_e = createGEZero(new_e);

            expr_t *e = (expr_t *)calloc( 1, sizeof(expr_t) );
            e->type = L_AND;
            e->l = exp;
            e->r = new_e;
            exp = e;

        }
    }
  
    return exp;
}

expr_t *solveExpressionTreeForVar(expr_t *exp, const char *var_name, Relation R){
    set<expr_t *> eqs;
    expr_t *rslt_exp;

    eqs = find_all_EQs_with_var(var_name, exp);
    if( eqs.empty() ){
        return NULL;
    }

    // we have to solve for all the ovars that are immediately solvable
    // and propagate the solutions to the other ovars, until everything is
    // solvable (assuming that this is always feasible).
    int i=0;
    while( true ){
        // If we tried to solve for 1000 output variables and still haven't
        // found a solution, probably something went wrong and we've entered
        // an infinite loop
        Q2J_ASSERT( i++ < 1000 );

        // If one of the equations includes this variable and no other output
        // variables, we solve that equation for the variable and return the solution.
        rslt_exp = solve_directly_solvable_EQ(exp, var_name, R);
        if( NULL != rslt_exp ){
            return rslt_exp;
        }

        // If R is a Set (instead of a Relation), there are no output variables,
        // we just can't solve it for some reason.
        if( R.is_set() ){
            return NULL;
        }

        // If control reached this point it means that all the equations
        // we are trying to solve contain output vars, so try to solve all
        // the other equations first that are solvable and replace these
        // output variables with the corresponding solutions.
        for(int i=0; i<R.n_out(); i++){
            const char *ovar = strdup(R.output_var(i+1)->char_name());

            // skip the variable we are trying to solve for.
            if( !strcmp(ovar, var_name) )
                continue;

            expr_t *tmp_exp = solve_directly_solvable_EQ(exp, ovar, R);
            if( NULL != tmp_exp ){
                substitute_exp_for_var(tmp_exp, ovar, exp);
            }
            free((void *)ovar);
        }
    }

    // control should never reach here
    Q2J_ASSERT(0);
    return NULL;
}


expr_t *findParent(expr_t *root, expr_t *node){
    expr_t *tmp;

    if( NULL == root )
        return NULL;

    switch( root->type ){
        case INTCONSTANT:
        case IDENTIFIER:
            return NULL;

        default:
            if( ((NULL != root->l) && (root->l == node)) || ((NULL != root->r) && (root->r == node)) )
                return root;
            // If this node is not the parent, it might still be an ancestor, so go deeper.
            tmp = findParent(root->r, node);
            if( NULL != tmp ) return tmp;
            tmp = findParent(root->l, node);
            return tmp;
    }

    return NULL;
}

// This function assumes that all variables will be kids of a MUL.
// This assumption is true for all contraint equations that were
// generated by omega.
expr_t *findParentOfVar(expr_t *root, const char *var_name){
    expr_t *tmp;

    switch( root->type ){
        case INTCONSTANT:
            return NULL;

        case MUL:
            if( ((IDENTIFIER == root->l->type) && !strcmp(var_name, root->l->value.name)) ||
                ((IDENTIFIER == root->r->type) && !strcmp(var_name, root->r->value.name))   ){
                return root;
            }
            return NULL;

        default:
            tmp = findParentOfVar(root->r, var_name);
            if( NULL != tmp ) return tmp;
            tmp = findParentOfVar(root->l, var_name);
            return tmp;
    }
    return NULL;
}

void multiplyTreeByConstant(expr_t *exp, int c){

    if( NULL == exp ){
        return;
    }

    switch( exp->type ){
        case IDENTIFIER:
            return;

        case INTCONSTANT:
            exp->value.int_const *= c;
            break;

        case ADD:
            multiplyTreeByConstant(exp->l, c);
            multiplyTreeByConstant(exp->r, c);
            break;

        case MUL:
        case DIV:
            multiplyTreeByConstant(exp->l, c);
            break;
        default:
            fprintf(stderr,"ERROR: multiplyTreeByConstant() Unknown node type: \"%d\"\n",exp->type);
            Q2J_ASSERT(0);
    }
    return;
}


static void substitute_exp_for_var(expr_t *exp, const char *var_name, expr_t *root){
    expr_t *eq_exp, *new_exp, *mul, *parent;
    set<expr_t *> cnstr, ges;

    cnstr = find_all_EQs_with_var(var_name, root);
    ges = find_all_GEs_with_var(var_name, root);
    cnstr.insert(ges.begin(), ges.end());
    
    set<expr_t *>::iterator e_it;
    for(e_it=cnstr.begin(); e_it!=cnstr.end(); e_it++){
        int c;
        eq_exp = *e_it;

        // Get the coefficient of the variable and multiply the
        // expression with it so we replace the whole MUL instead
        // of hanging the expression off the MUL.
        c = getVarCoeff(eq_exp, var_name);
        new_exp = copy_tree(exp);
        multiplyTreeByConstant(new_exp, c);
        
        // Find the MUL and its parent and do the replacement.
        mul = findParentOfVar(eq_exp, var_name);
        parent = findParent(eq_exp, mul);
        Q2J_ASSERT( NULL != parent );
        if( parent->l == mul ){
            parent->l = new_exp;
        }else if( parent->r == mul ){
            parent->r = new_exp;
        }else{
            Q2J_ASSERT(0);
        }

    }
    return;
}


static expr_t *solve_directly_solvable_EQ(expr_t *exp, const char *var_name, Relation R){
    set<expr_t *> eqs;
    expr_t *eq_exp, *rslt_exp;

    eqs = find_all_EQs_with_var(var_name, exp);
    set<expr_t *>::iterator e_it;
    for(e_it=eqs.begin(); e_it!=eqs.end(); e_it++){
        int exp_has_output_vars = 0;

        eq_exp = *e_it;
        rslt_exp = solveConstraintForVar(eq_exp, var_name);

        if( R.is_set() )
            return rslt_exp;
  
        for(int i=0; i<R.n_out(); i++){
            const char *ovar = R.output_var(i+1)->char_name();
            if( expr_tree_contains_var(rslt_exp, ovar) ){
                exp_has_output_vars = 1;
                break;
            }
        }

        if( !exp_has_output_vars ){
            return rslt_exp;
        }
    }
    return NULL;
}


////////////////////////////////////////////////////////////////////////////////
//
expr_t *find_EQ_with_var(const char *var_name, expr_t *exp){
    expr_t *tmp_r, *tmp_l;

    switch( exp->type ){
        case IDENTIFIER:
            if( !strcmp(var_name, exp->value.name) )
                return exp; // return something non-NULL
            return NULL;

        case INTCONSTANT:
            return NULL;

        case L_OR:
            // If you find it in both legs, panic
            tmp_l = find_EQ_with_var(var_name, exp->l);
            tmp_r = find_EQ_with_var(var_name, exp->r);
            if( (NULL != tmp_l) && (NULL != tmp_r) ){
                fprintf(stderr,"ERROR: find_EQ_with_var(): variable \"%s\" is not supposed to be in more than one conjuncts.\n",var_name);
                exit( -1 );
            }
            // otherwise proceed normaly
            if( NULL != tmp_l ) return tmp_l;
            if( NULL != tmp_r ) return tmp_r;
            return NULL;

        case L_AND:
        case ADD:
        case MUL:
            // If you find it in either leg, return a non-NULL pointer
            tmp_l = find_EQ_with_var(var_name, exp->l);
            if( NULL != tmp_l ) return tmp_l;

            tmp_r = find_EQ_with_var(var_name, exp->r);
            if( NULL != tmp_r ) return tmp_r;

            return NULL;

        case EQ_OP:
            // If you find it in either leg, return this EQ
            if( NULL != find_EQ_with_var(var_name, exp->l) )
                return exp;
            if( NULL != find_EQ_with_var(var_name, exp->r) )
                return exp;
            return NULL;

        case GE: // We are not interested on occurances of the variable in GE relations.
        default:
            return NULL;
    }
    return NULL;
}


////////////////////////////////////////////////////////////////////////////////
//
static inline set<expr_t *> find_all_EQs_with_var(const char *var_name, expr_t *exp){
    return find_all_constraints_with_var(var_name, exp, EQ_OP);
}

////////////////////////////////////////////////////////////////////////////////
//
static inline set<expr_t *> find_all_GEs_with_var(const char *var_name, expr_t *exp){
    return find_all_constraints_with_var(var_name, exp, GE);
}

////////////////////////////////////////////////////////////////////////////////
//
static set<expr_t *> find_all_constraints_with_var(const char *var_name, expr_t *exp, int constr_type){
    set<expr_t *> eq_set;
    set<expr_t *> tmp_r, tmp_l;

    if( NULL == exp )
        return eq_set;

    switch( exp->type ){
        case IDENTIFIER:
            if( !strcmp(var_name, exp->value.name) ){
                eq_set.insert(exp); // just so we return a non-empty set
            }
            return eq_set;

        case INTCONSTANT:
            return eq_set;

        case L_OR:
            // If you find it in both legs, panic
            tmp_l = find_all_constraints_with_var(var_name, exp->l, constr_type);
            tmp_r = find_all_constraints_with_var(var_name, exp->r, constr_type);
            if( !tmp_l.empty() && !tmp_r.empty() ){
                fprintf(stderr,"\nERROR: find_all_constraints_with_var(): variable \"%s\" is not supposed to be in more than one conjuncts.\n", var_name);
                fprintf(stderr,"exp->l : %s\n",expr_tree_to_str(exp->l));
                fprintf(stderr,"exp->r : %s\n\n",expr_tree_to_str(exp->r));
                Q2J_ASSERT( 0 );
            }
            // otherwise proceed normaly
            if( !tmp_l.empty() ) return tmp_l;
            if( !tmp_r.empty() ) return tmp_r;
            return eq_set;

        case L_AND:
            // Merge the sets of whatever you find in both legs
            eq_set = find_all_constraints_with_var(var_name, exp->l, constr_type);
            tmp_r  = find_all_constraints_with_var(var_name, exp->r, constr_type);
            eq_set.insert(tmp_r.begin(), tmp_r.end());

            return eq_set;

        case ADD:
        case MUL:
            // If you find it in either leg, return a non-empty set
            tmp_l = find_all_constraints_with_var(var_name, exp->l, constr_type);
            if( !tmp_l.empty() ) return tmp_l;

            tmp_r = find_all_constraints_with_var(var_name, exp->r, constr_type);
            if( !tmp_r.empty() ) return tmp_r;

            return eq_set;

        case EQ_OP:
            // Only look deeper if the caller wants EQs
            if( EQ_OP == constr_type ){
                // If you find it in either leg, return this EQ
                tmp_l = find_all_constraints_with_var(var_name, exp->l, constr_type);
                tmp_r = find_all_constraints_with_var(var_name, exp->r, constr_type);
                if(  !tmp_l.empty() || !tmp_r.empty() ){
                    eq_set.insert(exp);
                }
            }
            return eq_set;

        case GE:
            // Only look deeper if the caller wants GEQs
            if( GE == constr_type ){
                // If you find it in either leg, return this EQ
                tmp_l = find_all_constraints_with_var(var_name, exp->l, constr_type);
                tmp_r = find_all_constraints_with_var(var_name, exp->r, constr_type);
                if(  !tmp_l.empty() || !tmp_r.empty() ){
                    eq_set.insert(exp);
                }
            }
            return eq_set;

        default:
            return eq_set;
    }
    return eq_set;
}


////////////////////////////////////////////////////////////////////////////////
//
void print_actual_parameters(dep_t *dep, expr_t *rel_exp, int type){
    Relation R = copy(*(dep->rel));
    set <const char *> vars_in_bounds;

    int dst_count = R.n_inp();
    for(int i=0; i<dst_count; i++){
        const char *var_name = strdup(R.input_var(i+1)->char_name());
        vars_in_bounds.insert(var_name);
    }

    dst_count = R.n_out();
    for(int i=0; i<dst_count; i++){
        if( i ) printf(", ");
        const char *var_name = strdup(R.output_var(i+1)->char_name());

        expr_t *solution = solveExpressionTreeForVar(copy_tree(rel_exp), var_name, R);
        if( NULL != solution )
            printf("%s", expr_tree_to_str(solution));
        else
            printf("%s", find_bounds_of_var(copy_tree(rel_exp), var_name, vars_in_bounds, R));
        free((void *)var_name);
    }
    return;
}

////////////////////////////////////////////////////////////////////////////////
//
expr_t *cnstr_to_tree(Constraint_Handle cnstr, int cnstr_type){
    expr_t *e_add, *root = NULL;
    expr_t *e_cns;
    
    for(Constr_Vars_Iter cvi(cnstr); cvi; cvi++){
        expr_t *e_cns = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_cns->type = INTCONSTANT;
        e_cns->value.int_const = (*cvi).coef;

        expr_t *e_var = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_var->type = IDENTIFIER;
        e_var->value.name = strdup( (*cvi).var->char_name() );

        expr_t *e_mul = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_mul->type = MUL;
        e_mul->l = e_cns;
        e_mul->r = e_var;

        // In the first iteration set the multiplication node to be the root
        if( NULL == root ){
            root = e_mul;
        }else{
            expr_t *e_add = (expr_t *)calloc( 1, sizeof(expr_t) );
            e_add->type = ADD;
            e_add->l = root;
            e_add->r = e_mul;
            root = e_add;
        }

    }
    // Add the constant
    if( cnstr.get_const() ){
        e_cns = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_cns->type = INTCONSTANT;
        e_cns->value.int_const = cnstr.get_const();

        e_add = (expr_t *)calloc( 1, sizeof(expr_t) );
        e_add->type = ADD;
        e_add->l = root;
        e_add->r = e_cns;
        root = e_add;
    }

    // Add a zero on the other size of the constraint (EQ, GEQ)
    e_cns = (expr_t *)calloc( 1, sizeof(expr_t) );
    e_cns->type = INTCONSTANT;
    e_cns->value.int_const = 0;

    e_add = (expr_t *)calloc( 1, sizeof(expr_t) );
    e_add->type = cnstr_type;
    e_add->l = root;
    e_add->r = e_cns;
    root = e_add;

    return root;
}


inline expr_t *eq_to_tree(EQ_Handle e){
    return cnstr_to_tree(e, EQ_OP);
}

inline expr_t *geq_to_tree(GEQ_Handle ge){
    return cnstr_to_tree(ge, GE);
}


expr_t *conj_to_tree( Conjunct *conj ){
    expr_t *root = NULL;

    for(EQ_Iterator ei = conj->EQs(); ei; ei++) {
        expr_t *eq_root = eq_to_tree(*ei);

        if( NULL == root ){
            root = eq_root;
        }else{
            expr_t *e_and = (expr_t *)calloc( 1, sizeof(expr_t) );
            e_and->type = L_AND;
            e_and->l = root;
            e_and->r = eq_root;
            root = e_and;
        }

    }

    for(GEQ_Iterator gi = conj->GEQs(); gi; gi++) {
        expr_t *geq_root = geq_to_tree(*gi);

        if( NULL == root ){
            root = geq_root;
        }else{
            expr_t *e_and = (expr_t *)calloc( 1, sizeof(expr_t) );
            e_and->type = L_AND;
            e_and->l = root;
            e_and->r = geq_root;
            root = e_and;
        }
    }

    return root;
}


expr_t *relation_to_tree( Relation R ){
    expr_t *root = NULL;

    if( R.is_null() )
        return NULL;

    for(DNF_Iterator di(R.query_DNF()); di; di++) {
        expr_t *conj_root = conj_to_tree( *di );

        if( NULL == root ){
            root = conj_root;
        }else{
            expr_t *e_or = (expr_t *)calloc( 1, sizeof(expr_t) );
            e_or->type = L_OR;
            e_or->l = root;
            e_or->r = conj_root;
            root = e_or;
        }
    }
    return root;
}


static set<expr_t *> findAllConjunctions(expr_t *exp){
    set<expr_t *> eq_set;
    set<expr_t *> tmp_r, tmp_l;

    if( NULL == exp )
        return eq_set;

    switch( exp->type ){
        case L_OR:
            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            tmp_l = findAllConjunctions(exp->l);
            if( !tmp_l.empty() ){
                eq_set.insert(tmp_l.begin(), tmp_l.end());
            }else{
                eq_set.insert(exp->l);
            }

            tmp_r = findAllConjunctions(exp->r);
            if( !tmp_r.empty() ){
                eq_set.insert(tmp_r.begin(), tmp_r.end());
            }else{
                eq_set.insert(exp->r);
            }

            return eq_set;

        default:
            tmp_l = findAllConjunctions(exp->l);
            eq_set.insert(tmp_l.begin(), tmp_l.end());
            tmp_r = findAllConjunctions(exp->r);
            eq_set.insert(tmp_r.begin(), tmp_r.end());
            return eq_set;
        
    }

}


void findAllConstraints(expr_t *tree, set<expr_t *> &eq_set){

    if( NULL == tree )
        return;

    switch( tree->type ){
        case EQ_OP:
        case GE:
            eq_set.insert(tree);
            break;

        default:
            findAllConstraints(tree->l, eq_set);
            findAllConstraints(tree->r, eq_set);
            break;
    }
    return;
}

////////////////////////////////////////////////////////////////////////////////
//
void findAllVars(expr_t *e, set<string> &var_set){

    if( NULL == e )
        return;

    switch( e->type ){
        case IDENTIFIER:
            var_set.insert(e->value.name);
            break;

        default:
            findAllVars(e->l, var_set);
            findAllVars(e->r, var_set);
            break;
    }
    return ;
}


        
    
void tree_to_omega_set(expr_t *tree, Constraint_Handle &handle, map<string, Variable_ID> all_vars, int sign){
    int coef;
    Variable_ID v = NULL;
    string var_name;
    map<string, Variable_ID>::iterator v_it;

    if( NULL == tree ){
        return;
    }

    switch( tree->type ){
        case INTCONSTANT:
            coef = tree->value.int_const;
            handle.update_const(sign*coef);
            break;

        case MUL:
            Q2J_ASSERT( (tree->l->type == INTCONSTANT && tree->r->type == IDENTIFIER) || (tree->r->type == INTCONSTANT && tree->l->type == IDENTIFIER) );
            if( tree->l->type == INTCONSTANT ){
                coef = tree->l->value.int_const;
                var_name = tree->r->value.name;
                for(v_it=all_vars.begin(); v_it!=all_vars.end(); v_it++){
                    if( !v_it->first.compare(var_name) ){
                        v = v_it->second;
                        break;
                    }
                }
            }else{
                coef = tree->r->value.int_const;
                var_name = tree->l->value.name;
                for(v_it=all_vars.begin(); v_it!=all_vars.end(); v_it++){
                    if( !v_it->first.compare(var_name) ){
                        v = v_it->second;
                        break;
                    }
                }
            }
            Q2J_ASSERT( NULL != v );
            handle.update_coef(v,sign*coef);
            break;

        default:
            tree_to_omega_set(tree->r, handle, all_vars, sign);
            tree_to_omega_set(tree->l, handle, all_vars, sign);
            break;
    }
    return;

}

expr_t *simplify_constraint_based_on_execution_space(expr_t *tree, Relation S_es){
    Relation S_rslt;
    set<expr_t *> e_set;
    set<expr_t *>::iterator e_it;

    findAllConstraints(tree, e_set);

    // For every constraint in the tree
    for(e_it=e_set.begin(); e_it!=e_set.end(); e_it++){
        map<string, Variable_ID> all_vars;
        Relation S_tmp;
        expr_t *e = *e_it;

        // Create a new Set
        set<string>vars;
        findAllVars(e, vars);
        S_tmp = Relation(S_es.n_set());

        for(int i=1; i<=S_es.n_set(); i++){
            string var_name = S_es.set_var(i)->char_name();
            S_tmp.name_set_var( i, strdup(var_name.c_str()) );
            all_vars[var_name] = S_tmp.set_var( i );
        }

        // Deal with the remaining variables in the expression tree,
        // which should all be global.
        set<string>::iterator v_it;
        for(v_it=vars.begin(); v_it!=vars.end(); v_it++){
            string var_name = *v_it;

            // If it's not one of the vars we've already dealt with
            if( all_vars.find(var_name) == all_vars.end() ){
                // Make sure this variable is global
                map<string, Free_Var_Decl *>::iterator g_it;
                g_it = global_vars.find(var_name);
                Q2J_ASSERT( g_it != global_vars.end() );
                // And get a reference to the local version of the variable in S_tmp.
                all_vars[var_name] = S_tmp.get_local(g_it->second);
            }
        }

        F_And *S_root = S_tmp.add_and();
        Constraint_Handle handle;
        if( EQ_OP == e->type ){
            handle = S_root->add_EQ();
        }else if( GE == e->type ){
            handle = S_root->add_GEQ();
        }else{
            Q2J_ASSERT(0);
        }

        // Add the two sides of the constraint to the Omega set
        tree_to_omega_set(e->l, handle, all_vars, 1);
        tree_to_omega_set(e->r, handle, all_vars, -1);
        S_tmp.simplify();

        // Calculate S_exec_space - ( S_exec_space ^ S_tmp )
        Relation S_intrs = Intersection(copy(S_es), copy(S_tmp));
        Relation S_diff = Difference(copy(S_es), S_intrs);
        S_diff.simplify();

        // If it's not FALSE, then throw S_tmp in S_rslt
        if( S_diff.is_upper_bound_satisfiable() || S_diff.is_lower_bound_satisfiable() ){
            if( S_rslt.is_null() ){
                S_rslt = copy(S_tmp);
            }else{
                S_rslt = Intersection(S_rslt, S_tmp);
            }
        }

        S_tmp.Null();
        S_diff.Null();
    }

    return relation_to_tree( S_rslt );
}


////////////////////////////////////////////////////////////////////////////////
//
// WARNING: this function is destructive.  It actually removes nodes from the tree
// and deletes them altogether. In many cases you will need to pass a copy of the
// tree to this function.
static list< pair<expr_t *,Relation> > simplify_conditions_and_split_disjunctions(Relation R, Relation S_es){
    stringstream ss;
    set<expr_t *> simpl_conj;

    list< pair<expr_t *, Relation> > tmp, result;
    list< pair<expr_t *, Relation> >::iterator cj_it;

    for(DNF_Iterator di(R.query_DNF()); di; di++) {
        pair<expr_t *, Relation> p;
        Relation tmpR = Relation(copy(R), *di);
        tmpR.simplify();
        p.first = relation_to_tree(tmpR);
        tmpR.simplify();
        p.second = tmpR;
        tmp.push_back(p);
    }

    // Eliminate the conjunctions that are covered by the execution space
    // and simplify the remaining ones
    for(cj_it = tmp.begin(); cj_it != tmp.end(); cj_it++){
        pair<expr_t *, Relation> new_p;
        pair<expr_t *, Relation> p = *cj_it;
        expr_t *cur_exp = p.first;

        int dst_count = R.n_out();
        for(int i=0; i<dst_count; i++){
            const char *ovar = strdup(R.output_var(i+1)->char_name());
            // If we find the variable in an EQ then we solve for the variable and
            // substitute the solution for the variable everywhere in the conjunction.
            expr_t *solution = solveExpressionTreeForVar(cur_exp, ovar, R);
            if( NULL != solution ){
                substitute_exp_for_var(solution, ovar, cur_exp);
            }else{
                // If the variable is in no EQs but it's in GEs, we have to use transitivity
                // to eliminate it.  For example: X-a>=0 && b-X-1>=0 => b-1>=X && X>=a => b-1>=a
                cur_exp = eliminateVarUsingTransitivity(cur_exp, ovar, R);
            }
            free((void *)ovar);
        }
        cur_exp = simplify_constraint_based_on_execution_space(cur_exp, S_es);

        new_p.first = cur_exp;
        new_p.second = p.second;
        result.push_back(new_p);
    }

    return result;
}

static StarPU_codelet *starpu_call_to_codelet( char *call_name){
    StarPU_codelet_list *l;
    for(l = codelet_list; l != NULL; l = l->prev)
    {
        if(strcmp(l->cl->name, call_name) == 0)
        {	    
            return l->cl;
        }
    }
    
    fprintf(stderr, "Unable to find the codelet corresponding to %s\n", call_name);
    exit(-1);
    return NULL;
}   

int starpu_print_statement(node_t *node, StarPU_task *task, int flag)
{
    int flag_print = 1;
    node_t *tmp;

    switch(node->type){
    case BLOCK:
	printf("{\n    ");
	for(tmp = node->u.block.first; NULL!=tmp; tmp=tmp->next)
	{
	    if(starpu_print_statement(tmp, task, flag) == 1)
		printf(";\n    ");
	}
	printf("\n}\n");	
	break;
    case COMMA_EXPR:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(", ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case S_U_MEMBER:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(".");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case GE:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(">= ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case LE:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("<= ");	
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case GT:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("> ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case LT:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("< ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case RSHIFT:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(">> ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case LSHIFT:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("<< ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case B_OR:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("| ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case B_AND:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("& ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case B_XOR:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("^ ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case MOD:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("% ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case DIV:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("/ ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case MUL:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("* ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case SUB:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("- ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case ADD:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("+ ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case EXPR:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case FCALL:
	if(strcmp((node->u.kids.kids[0])->u.var_name, "starpu_codelet_unpack_args") != 0)
	{
	    int i;
	    printf("%s(", (node->u.kids.kids[0])->u.var_name);
	    for(i = 1; i<node->u.kids.kid_count-1; i++)
	    {
		starpu_print_statement(node->u.kids.kids[i], task, flag);
		printf(", ");
	    }
	    if(node->u.kids.kid_count > 1)
		starpu_print_statement(node->u.kids.kids[node->u.kids.kid_count-1], task, flag);
	    printf(")");
	}
	else
            flag_print = 0;
	break;
    case ARRAY:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	for(int i=1; i<node->u.kids.kid_count; i++)
	{
	    printf("[");
	    starpu_print_statement(node->u.kids.kids[i], task, flag);
	    printf("]");
	}
	break;
    case COND:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("?");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	printf(":");
	starpu_print_statement(node->u.kids.kids[2], task, flag);
	break;
    case ASSIGN:
	if((node->u.kids.kids[1])->type != FCALL || 
	   strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
	{
	    starpu_print_statement(node->u.kids.kids[0], task, flag);
	    printf("= ");
	    starpu_print_statement(node->u.kids.kids[1], task, flag);	    
	}
	else
            flag_print = 0;
	break;
    case BANG:
	printf("!");
	break;
    case TILDA:
	printf("~");
	break;
    case MINUS:
	printf("-");
	break;
    case PLUS:
	printf("+");
	break;
    case STAR:
	printf("*");
	break;
    case ADDR_OF:
	printf("&");
	break;
    case WHILE:
	printf("while(");
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(")\n");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case IF:
	printf("if(");
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf(")\n");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	if(node->u.kids.kid_count == 3)
	{
	    if((node->u.kids.kids[1])->type != BLOCK)
		printf(";");
	    printf("\nelse\n");
	    starpu_print_statement(node->u.kids.kids[2], task, flag);
	}
	break;
    case DO:
	printf("do\n");
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("\nwhile(");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	printf(")\n");
	break;
    case FOR:
	printf("for(");
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("; ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	printf("; ");
	starpu_print_statement(node->u.kids.kids[2], task, flag);
	printf(")\n");
	starpu_print_statement(node->u.kids.kids[3], task, flag);
	break;
    case RETURN:
	printf("return ");
	break;
    case GOTO:
	printf("goto ");
	break;
    case BREAK:
	printf("break ");
	break;
    case CONTINUE:
	printf("continue ");
	break;
    case L_AND:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("&& ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case L_OR:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("|| ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case NE_OP:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("!= ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case EQ_OP:
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("== ");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	break;
    case CHAR:
	printf("char ");
	break;
    case CONST:
	printf("const ");
	break;
    case DOUBLE:
	printf("double ");
	break;
    case ENUM:
	printf("enum ");
	break;
    case EXTERN:
	printf("extern ");
	break;
    case FLOAT:
	printf("float ");
	break;
    case INT:
	printf("int ");
	break;
    case LONG:
	printf("long ");
	break;
    case INT16:
	printf("int16_t ");
	break;
    case INT32:
	printf("int32_t ");
	break;
    case INT64:
	printf("int64_t ");
	break;
    case UINT8:
	printf("uint8_t ");
	break;
    case UINT16:
	printf("uint16_t ");
	break;
    case UINT32:
	printf("uint32_t ");
	break;
    case UINT64:
	printf("uint64_t ");
	break;
    case SHORT:
	printf("short ");
	break;
    case SIGNED:
	printf("signed ");
	break;
    case STATIC:
	printf("static ");
	break;
    case STRUCT:
	printf("struct ");
	break;
    case UNION:
	printf("union ");
	break;
    case UNSIGNED:
	printf("unsigned ");
	break;
    case VOID:
	printf("void ");
	break;
    case PLASMA_COMPLEX32_T:
	printf("PLASMA_Complex32_t ");
	break;
    case PLASMA_COMPLEX64_T:
	printf("PLASMA_Complex64_t ");
	break;
    case PLASMA_ENUM:
	printf("PLASMA_enum ");
	break;
    case PLASMA_SEQUENCE:
	printf("PLASMA_sequence ");
	break;
    case PLASMA_DESC:
	printf("PLASMA_desc ");
	break;
    case PLASMA_REQUEST:
	printf("PLASMA_request ");
	break;
    case IDENTIFIER:
    {
	StarPU_variable_list *vl;
	StarPU_variable_list *decl;
	int printed = 1;
	int decl_flag = 1;
	int cuda_flow = 0;
	if(flag == FLAG_CUDA)
	    decl = task->cudadecl;
	else
	    decl = task->cpudecl;
	for(; decl!=NULL; decl = decl->next)
	{
	    if(strcmp(decl->new_name, node->u.var_name) == 0)
	    {
		decl_flag = 0;
	    }
	}

	if(decl_flag == 1) 
	{
	    // Declaration
	    symbol_t *s;

	    decl = (StarPU_variable_list*) calloc(1, sizeof(struct StarPU_variable_list_t));
	    if(FLAG_CUDA==flag)
	    {
		vl = task->cudadecl;
		decl->new_name = strdup(node->u.var_name);
	    }
	    else
	    {
		vl = task->cpudecl;
		decl->new_name = strdup(node->u.var_name);
	    }
	    decl->next = NULL;


	    for(; vl != NULL && vl->next != NULL; vl = vl->next);
	    if(vl == NULL)
	    {
		if(FLAG_CUDA == flag)
		    task->cudadecl = decl;
		else
		    task->cpudecl = decl;
	    }
	    else
		vl->next = decl;
	    // c'est une variable globale - on ne la declare pas
	    for(s=task->symtab->symbols; s!=NULL; s=s->next)
	    {
		if(strcmp(s->var_name, node->u.var_name) == 0)
		{
		    printed = 0;
		}
	    }
	    // c'est une variable de flot - on ne la declare pas 
	    for(vl=task->vl; vl!=NULL; vl = vl->next)
	    {
		if(vl->new_name != NULL &&
		   strcmp(vl->new_name, node->u.var_name) == 0 && 
		   flag == FLAG_CPU)  
		    printed = 0;
		else if(vl->cuda_name != NULL &&
			strcmp(vl->cuda_name, node->u.var_name) == 0 &&
			flag == FLAG_CUDA)
		    printed = 0;
	    }
	    // c'est une variable globale?
	    for(vl=task->glob; vl!=NULL; vl = vl->next)
	    {
		if(flag == FLAG_CPU &&
		   vl->new_name != NULL &&
		   strcmp(vl->new_name, node->u.var_name) == 0)
		    printed = 0;
		else if(flag == FLAG_CUDA && 
			vl->cuda_name != NULL &&
			strcmp(vl->cuda_name, node->u.var_name) == 0)
		{
		    printed = 0;
		}

	    }
/*	    if(printed == 1)
		fprintf(stdout, "%s %s",
			node->var_type, 
			node->pointer == 1 ? "*" : "\0");

/*/
	    for(s=node->symtab->symbols; s!=NULL; s=s->next)
	    {
		if(strcmp(node->u.var_name, s->var_name) == 0 && printed)
		{
		    printf("%s ", s->var_type);
		}
	    }
	}
	if(printed && node->cast == 1)
	    printf("(%s) ", node->var_type);

	for(vl = task->glob; vl != NULL; vl = vl->next)
	{
	    if(flag == FLAG_CPU && strcmp(vl->new_name, node->u.var_name) == 0 && printed)
	    {
		printf("%s ", vl->call_name);
		printed = 0;
	    }
	    if(flag == FLAG_CUDA && strcmp(vl->cuda_name, node->u.var_name) == 0 && printed )
	    {
		printf("%s ", vl->call_name); 
		printed = 0;
	    }
	}

	for(vl = task->vl; vl != NULL; vl = vl->next)
	{
	    if(flag == FLAG_CUDA && strcmp(vl->cuda_name, node->u.var_name) == 0 && printed)
	    {
		printf("%s", printed == 1 ? vl->new_name : "\0");
		cuda_flow = 1;
	    }
	}
	
	if(flag == FLAG_CPU || cuda_flow == 0) 
	    printf("%s", printed == 1 ? node->u.var_name : "\0");
	flag_print = printed;
	break;
    }
    case INTCONSTANT:
	printf("%lld ", node->const_val.i64_value);
	break;
    case FLOATCONSTANT:
	printf("%lf ", node->const_val.f64_value);
	break;
    case STRING_LITERAL:
	printf("%s", node->const_val.str);
	break;
    case ADD_ASSIGN:
        if((node->u.kids.kids[1])->type != FCALL ||
           strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
        {
            starpu_print_statement(node->u.kids.kids[0], task, flag);
            printf("+= ");
            starpu_print_statement(node->u.kids.kids[1], task, flag);
        }
	else 
	    flag_print = 0;
	break;
    case MUL_ASSIGN:
        if((node->u.kids.kids[1])->type != FCALL ||
           strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
        {
            starpu_print_statement(node->u.kids.kids[0], task, flag);
            printf("*= ");
            starpu_print_statement(node->u.kids.kids[1], task, flag);
        }
	else
	    flag_print = 0;
	break;
    case DIV_ASSIGN:
        if((node->u.kids.kids[1])->type != FCALL ||
           strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
        {
            starpu_print_statement(node->u.kids.kids[0], task, flag);
            printf("/= ");
            starpu_print_statement(node->u.kids.kids[1], task, flag);
        }
	else
            flag_print = 0;
	break;
    case MOD_ASSIGN:
	if((node->u.kids.kids[1])->type != FCALL ||
           strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
        {
            starpu_print_statement(node->u.kids.kids[0], task, flag);
            printf("%= ");
            starpu_print_statement(node->u.kids.kids[1], task, flag);
        }
	else
            flag_print = 0;
	break;
    case SUB_ASSIGN:
        if((node->u.kids.kids[1])->type != FCALL ||
           strcmp(((node->u.kids.kids[1])->u.kids.kids[0])->u.var_name, "STARPU_MATRIX_GET_PTR") != 0)
        {
            starpu_print_statement(node->u.kids.kids[0], task, flag);
            printf("-= ");
            starpu_print_statement(node->u.kids.kids[1], task, flag);
        }
	else
            flag_print = 0;
	break;
    case PTR_OP:
	printf("->");
	break;
    case INC_OP:
	printf("++");
	break;
    case DEC_OP:
	printf("--");
	break;
    case SIZEOF:
	printf("sizeof(");
	if(node->u.kids.kid_count == 1)
	    starpu_print_statement(node->u.kids.kids[0], task, flag);
	else
	    printf("%s", node->u.var_name);
	printf(")");
    case SWITCH:
	printf("switch(");
	starpu_print_statement(node->u.kids.kids[0], task, flag);
	printf("){\n");
	starpu_print_statement(node->u.kids.kids[1], task, flag);
	printf("}\n");
    case EMPTY:
    case ENTRY:
    case EXIT:	
    case DEREF:
    default: break;
    }
    return flag_print;
}


void starpu_print_cpu_kernel_to_function(char *fun_name)
{
    StarPU_translation_list *l;
    node_t *kernel;
    for(l = trans_list; l != NULL; l = l->prev)
    {
	if(strcmp(fun_name, l->tr->name) == 0)
	    break;
    }
    if(l == NULL)
    {
        fprintf(stderr, "Unable to find the function %s\n", fun_name);
        exit(-1);
    }
//    kernel = starpu_find_cpu_kernel(l->tr->node);
    
    // TODO : 
    // Print the function with the parameters
}

void starpu_print_body_cpu(StarPU_task *src_task)
{    
    printf("BODY\n\n");
    starpu_print_statement(src_task->cpu->node, src_task, FLAG_CPU);
    printf("\nEND\n\n");   
}

void starpu_print_body_cuda(StarPU_task *src_task)
{
    printf("BODY CUDA\n\n");
    starpu_print_statement(src_task->cuda->node, src_task, FLAG_CUDA);
    printf("\nEND\n\n");
}

////////////////////////////////////////////////////////////////////////////////
//

StarPU_task *starpu_find_task(char *tname)
{
    StarPU_task_list *tl;
    for(tl = starpu_task_list; tl != NULL; tl = tl->next)
    {
	if(strcmp(tl->t->name, tname) == 0)
	    return tl->t; 
    }
    fprintf(stderr, "Unable to find the starpu task %s\n", tname);
    exit(-1);
    return NULL;
}

void print_body(task_t *src_task){
    node_t *task_node = src_task->task_node;

#ifdef QUARK_COMPILER
    printf("BODY\n\n");
    printf("%s\n", quark_tree_to_body(task_node));
    printf("\nEND\n\n");
#else
    
    starpu_print_body_cpu(starpu_find_task(src_task->task_name));
    starpu_print_body_cuda(starpu_find_task(src_task->task_name));
#endif
}


void print_header(){
    printf("extern \"C\" %%{\n"
           "/*\n"
           " *  Copyright (c) 2010\n"
           " *\n"
           " *  The University of Tennessee and The University\n"
           " *  of Tennessee Research Foundation.  All rights\n"
           " *  reserved.\n"
           " *\n"
           " * @precisions normal z -> s d c\n"
           " *\n"
           " */\n"
           "#define PRECISION_z\n"
           "\n"
           "#include <plasma.h>\n"
           "#include <core_blas.h>\n"
           "\n"
#ifndef QUARK_COMPILER
	   "#include <starpu.h>\n"
	   "#include \"magma.h\"\n"
	   "#include \"cublas.h\"\n"
	   "#include \"cuda.h\"\n"
#endif
           "#include \"dague.h\"\n"
           "#include \"data_distribution.h\"\n"
           "#include \"data_dist/matrix/precision.h\"\n"
           "#include \"data_dist/matrix/matrix.h\"\n"
           "#include \"dplasma/lib/memory_pool.h\"\n"
           "#include \"dplasma/lib/dplasmajdf.h\"\n"
           "\n"
           "#define BLKLDD(_desc, _k) (_desc).mb\n"
           "\n"
           "%%}\n\n");
}



////////////////////////////////////////////////////////////////////////////////
//
set<dep_t *> edge_map_to_dep_set(map<char *, set<dep_t *> > edges){
    map<char *, set<dep_t *> >::iterator edge_it;
    set<dep_t *> deps;

    for(edge_it = edges.begin(); edge_it != edges.end(); ++edge_it ){
        set<dep_t *> tmp;

        tmp = edge_it->second;
        deps.insert(tmp.begin(), tmp.end());
    }

    return deps;
}



#define EDGE_ANTI  0
#define EDGE_FLOW  1
#define EDGE_UNION 2

typedef struct tg_node tg_node_t;
typedef struct tg_edge{
    Relation *R;
    tg_node_t *dst;
    int type;
} tg_edge_t;

struct tg_node{
    char *task_name;
    list <tg_edge_t *> edges;
    Relation *cycle;
    tg_node(){
    }
};


////////////////////////////////////////////////////////////////////////////////
//
tg_node_t *find_node_in_graph(char *task_name, map<char *, tg_node_t *> task_to_node){
    map<char *, tg_node_t *>::iterator it;

    for(it=task_to_node.begin(); it!=task_to_node.end(); ++it){
        if( !strcmp(it->first, task_name) )
            return it->second;
    }

    return NULL;
}


////////////////////////////////////////////////////////////////////////////////
// We pass visited_nodes by referece because we want the caller to see the changes
// that the callee made, after the control returns to the caller.
void add_tautologic_cycles(tg_node_t *src_nd, set<tg_node_t *> &visited_nodes){
    int n_inp_var;
    Relation *newR;

    // If we are at an end node, add a null Relation and return.
    if( !src_nd->edges.size() ){
        src_nd->cycle = new Relation();
        return;
    }

    // Find a relation from an edge that starts from this node (any edge is fine).
    list <tg_edge_t *>::iterator it = src_nd->edges.begin();
    Relation *tmpR = (*it)->R;

    if( tmpR->is_null() ){
        abort();
    }
    if( tmpR->is_set() ){
        fprintf(stderr,"Strange Relation in edge from: %s to: %s\n",src_nd->task_name, (*it)->dst->task_name);
        abort();
    }

    // See how many input variables it has, that's the loop depth of the kernel.
    n_inp_var = tmpR->n_inp();
  
    // Create a new relation with that arity (number of variables).
    newR = new Relation(n_inp_var, n_inp_var); // Yes, n_inp_var both times

    // WARNING: This is needed by Omega. If you remove it you get strange
    // assert() calls being triggered inside the Omega library.

    (void)tmpR->print_with_subs_to_string(false);

    // Name all variables accordingly.
    for(int i=0; i<n_inp_var; i++){
        char *var_name = strdup( tmpR->input_var(i+1)->char_name() );
        newR->name_input_var(i+1, var_name );
        newR->name_output_var(i+1, var_name );
    }

    // Make each input variable be equal to the corresponding output variable
    // as in: {[V1,...,Vn] -> [V1',...,Vn] : V1=V1' && ... && Vn=Vn'}

    F_And *newR_root = newR->add_and();
    for(int i=0; i<n_inp_var; i++){
        Variable_ID ovar = newR->output_var(i+1);
        Variable_ID ivar = newR->input_var(i+1);
        EQ_Handle eq = newR_root->add_EQ();
        eq.update_coef(ovar,1);
        eq.update_coef(ivar,-1);
    }

    // Store it into the node
    src_nd->cycle = newR;

    // If there are other nodes we could get to from this node that have not already been
    // visited, then go to these nodes and add tautologic cycles to them.
    for (it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        tg_node_t *tmp_node = (*it)->dst;
        if( visited_nodes.find(tmp_node) == visited_nodes.end() ){
            visited_nodes.insert(tmp_node);
            add_tautologic_cycles(tmp_node, visited_nodes);
        }
    }

    return;
}

////////////////////////////////////////////////////////////////////////////////
// The following function is for debug purposes only.
void dump_graph(tg_node_t *src_nd, set<tg_node_t *> &visited_nodes){

    // If there are other nodes we could get to from this node that have not already been
    // visited, then go to them
    for (list<tg_edge_t *>::iterator it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        tg_node_t *tmp_node = (*it)->dst;

        fprintf(stderr, "%s -> %s\n",src_nd->task_name, tmp_node->task_name);

        if( visited_nodes.find(tmp_node) == visited_nodes.end() ){
            visited_nodes.insert(tmp_node);
            dump_graph(tmp_node, visited_nodes);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// We pass visited_nodes by referece because we want the caller to see the changes
// that the callee made, after the control returns to the caller.
void union_graph_edges(tg_node_t *src_nd, set<tg_node_t *> &visited_nodes){
    map<tg_node_t *, Relation> edge_aggregator;

    // For each edge, see if there are other edges with the same destination. If so, union them all together.
    for (list<tg_edge_t *>::iterator it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        Relation newR;
        tg_edge_t *tmp_edge = *it;

        // See if there is already an edge from this source to the same destination as "tmp_edge".
        map<tg_node_t *, Relation>::iterator edge_it = edge_aggregator.find(tmp_edge->dst);
        if( edge_it == edge_aggregator.end() ){
            newR = *(tmp_edge->R);
        }else{
            // we are going to overwrite edge_it->second in the next step anyway, so we can clobber it.
            newR = Union(edge_it->second, copy(*(tmp_edge->R)));
        }
        edge_aggregator[tmp_edge->dst] = newR;
    }

    // Clear the existing list of edges.
    for (list<tg_edge_t *>::iterator it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        free( *it );
    }
    src_nd->edges.clear();

    // Make the resulting edges of the unioning be the edges of this node.
    for (map<tg_node_t *, Relation>::iterator it = edge_aggregator.begin(); it != edge_aggregator.end(); it++){
        tg_edge_t *new_edge = (tg_edge_t *)calloc(1, sizeof(tg_edge_t));
        new_edge->type = EDGE_UNION;
        new_edge->dst = it->first;
        new_edge->R = new Relation(it->second);
        src_nd->edges.push_back(new_edge);
    }
    // reclaim some memory
    edge_aggregator.clear();

    // If there are other nodes we could get to from this node that have not already been
    // visited, then update their edges.
    for (list<tg_edge_t *>::iterator it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        tg_node_t *tmp_node = (*it)->dst;
        if( visited_nodes.find(tmp_node) == visited_nodes.end() ){
            visited_nodes.insert(tmp_node);
            union_graph_edges(tmp_node, visited_nodes);
        }
    }

    return;
}

////////////////////////////////////////////////////////////////////////////////
//
void compute_TC_of_transitive_relation_of_cycle(list<tg_node_t *> cycle_list){
    Relation transitiveR;
    tg_node_t *tmp_dst_nd, *tmp_src_nd, *cycle_start;
    list<tg_edge_t *>::iterator e_it;
    list<tg_node_t *>::iterator stack_it;

    stack_it=cycle_list.begin();

    // Store the begining of the list, we will need it later to close the cycle
    // and store the resulting Relation.
    cycle_start = *stack_it;

    // Initialize the source node to be the first element in the cycle.
    tmp_src_nd = cycle_start;
    stack_it++;
    // Slowly move down the cycle computing the transitive relation in the process.
    for(; stack_it!=cycle_list.end(); ++stack_it){
        // Store the next element in tmp_dst_nd
        tmp_dst_nd = *stack_it;
        for(e_it = tmp_src_nd->edges.begin(); e_it != tmp_src_nd->edges.end(); e_it++){
        tg_edge_t *tmp_edge = *e_it;
        // Find the edge that connects tmp_src_nd and tmp_dst_nd and compose
        if( tmp_edge->dst == tmp_dst_nd ){
            if( transitiveR.is_null() )
                transitiveR = *(tmp_edge->R);
            else
                transitiveR = Composition(copy(*(tmp_edge->R)), transitiveR);
            break;
        }
                }
                // progress the source down the cycle
                tmp_src_nd = tmp_dst_nd;
    }

    // Close the cycle by making the begining of the cycle also be the end.
    tmp_dst_nd = cycle_start;
    for(e_it = tmp_src_nd->edges.begin(); e_it != tmp_src_nd->edges.end(); e_it++){
        tg_edge_t *tmp_edge = *e_it;
        // Find the edge that connects the tmp_src_nd and tmp_dst_nd and compose
        if( tmp_edge->dst == tmp_dst_nd ){
            if( transitiveR.is_null() )
                transitiveR = *(tmp_edge->R);
            else
                transitiveR = Composition(copy(*(tmp_edge->R)), transitiveR);
            break;
        }
    }
    // Compute the transitive closure of this relation

//printf("  Before TC: ");
//transitiveR.print_with_subs(stdout);

    transitiveR = TransitiveClosure(transitiveR);

//printf("  After TC: ");
//transitiveR.print_with_subs(stdout);

    // Union the resulting relation with the relation stored in the cycle of this node
    cycle_start->cycle = new Relation( Union(*(cycle_start->cycle), transitiveR) );

    return;
}

////////////////////////////////////////////////////////////////////////////////
// Note: the use of the word "stack" in this function is a misnomer. We do not
// use an STL stack, but rather a list, because we want functionality that lists
// have and stacks don't (like begin/end iterators).  However, we only push
// elements in it from one side (push_back()) so it's filled up like a stack,
// although it's queried like a list. I guess it's really a queue.
//
// Note: We pass visited_nodes by value, so that the effect of the callee is _not_
// visible by the caller.
void compute_transitive_closure_of_all_cycles(tg_node_t *src_nd, set<tg_node_t *> visited_nodes, list<tg_node_t *> node_stack){
    static int depth;

    visited_nodes.insert(src_nd);
    node_stack.push_back(src_nd);

    for (list<tg_edge_t *>::iterator it = src_nd->edges.begin(); it != src_nd->edges.end(); it++){
        tg_node_t *next_node = (*it)->dst;

//printf("#%*s",depth,"");
//printf("%s -> %s\n",src_nd->task_name, next_node->task_name);

        // If the destination node of this edge has not been visited, jump to it and continue the
        // search for a cycle.  If the destination node is in our visited set, then we detected a
        // cycle, so we should compute the appropriate Relations and store them in the "cycle"
        // field of the corresponding nodes.
        if( visited_nodes.find(next_node) == visited_nodes.end() ){
            depth += 4;
            compute_transitive_closure_of_all_cycles(next_node, visited_nodes, node_stack);
            depth -= 4;
        }else{
            list<tg_node_t *>::iterator stack_it;

//printf("#%*s Found cycle.\n",depth,"");

            // Since next_node has already been visited, we hit a cycle.  Many cycles actually,
            // since each node in the cycle should be treated as the as the source of a
            // different cyclic transitive edge.
            tg_node_t *cycle_start = next_node;

            // Traverse the elements starting from the oldest (violating the stack semantics),
            // until we find "cycle_start".  All the elements before "cycle_start" are not
            // members of this cycle. We could instead pop the stack from the top and fill up
            // a (reversed) list of elements in the cycle, but that would destroy the stack,
            // and we need to keep it for the next iteration of the for() loop.
            stack_it=node_stack.begin();
            while( stack_it!=node_stack.end() && *stack_it!=cycle_start ){
                ++stack_it;
            }

            // At this point we should not have run out of stack and we should have
            // found the begining of the cycle.
            Q2J_ASSERT(stack_it!=node_stack.end() && *stack_it==cycle_start);

            // copy the elements of the cycle into a new list.
            list<tg_node_t *> cycle_list(stack_it, node_stack.end());

            // Compute the transitive Relation of the cycle (and then compute the
            // transitive closure of that) as many times as there are elements in
            // the cycle, changing the starting element of the cycle every time.
            do{
                compute_TC_of_transitive_relation_of_cycle(cycle_list);

                cycle_list.push_back( cycle_list.front() );
                cycle_list.pop_front();

            // when the start is the same element as it was when we started, we are done.
            }while( cycle_list.front() != cycle_start );
        }
    }

    return;
}


////////////////////////////////////////////////////////////////////////////////
//
Relation find_transitive_edge(tg_node_t *cur_nd, Relation Rt, Relation Ra, set<tg_node_t *> visited_nodes, list<Relation *> relation_fifo, tg_node_t *src_nd, tg_node_t *snk_nd, int just_started){
    static int debug_depth=0;

    visited_nodes.insert(cur_nd);

    // T <- Cycle(Nc) o T
    // Why does the algorithm say not to take the cycle for the starting node?

    // Add the node's cycle in the fifo, unless it's null. This will happen in
    // nodes with no outgoing edges (think DAGUE_OUT_A).
    if( !(cur_nd->cycle->is_null()) ){
        // If the ant-edge we are finalizing is a self edge (source==sink), then cur_nd will be equal
        // to snk_nd twice.  We will only put the cycle of that node in the fifo once, at the end.
        if ( !((cur_nd == snk_nd) && just_started) ){
            relation_fifo.push_back(cur_nd->cycle);
        }
    }

    // if Nc == Sink(Ea)
    if ( (cur_nd == snk_nd) && !just_started ){
//printf("Reached the sink: %s\n",snk_nd->task_name);
        // A U T
        Relation Rtrnsv;
        list<Relation *>::iterator rel_it = relation_fifo.begin();
        if( rel_it != relation_fifo.end() ){
            Rtrnsv = *(*rel_it);
            Rtrnsv.simplify();
//debug
//Rtrnsv.print_with_subs();
//printf("--->\n");
            rel_it++;
            for ( ; rel_it != relation_fifo.end(); rel_it++){
                Relation Rtmp = *(*rel_it);
                Rtmp.simplify();
//debug
//Rtmp.print_with_subs();
//printf("--->\n");
                Rtrnsv = Composition(Rtmp, Rtrnsv);
                Rtrnsv.simplify();
            }
//printf("||||\n");
        }
        Rt = Rtrnsv;
        if( Ra.is_null() ){
//printf("returning Rt\n");
            return(Rt);
        }else if( Rt.is_null() ){
//printf("returning Ra\n");
            return(Ra);
        }else{
            // the Union() function will clobber its arguments, but that's ok because they
            // were copies of stored Relations, they don't need to be remembered.
//printf("Computing the Union of:");
//Ra.print_with_subs();
//printf("And:");
//Ra.print_with_subs();
//printf("returning the union\n");
            Relation Ru = Union(Ra, Rt);
            return Ru;
        }
    }

    // foreach Edge Nc->Ni (with Relation Ri)
    for (list<tg_edge_t *>::iterator it = cur_nd->edges.begin(); it != cur_nd->edges.end(); it++){
        tg_node_t *next_node = (*it)->dst;
        Relation Rt_new;
        
//printf("%*s",debug_depth,"");
//printf("%s -> %s\n",cur_nd->task_name, next_node->task_name);

        // If the next node (Ni) has not already been visited, or if the source and sink of the edge we
        // are finalizing are the same (loop carried self edge) and the next node is that (source/sink) node.
        if( (visited_nodes.find(next_node) == visited_nodes.end()) || ((src_nd == snk_nd) && (next_node == snk_nd)) ){
            relation_fifo.push_back((*it)->R);
            
            debug_depth += 4;
            Ra = find_transitive_edge(next_node, Rt_new, Ra, visited_nodes, relation_fifo, src_nd, snk_nd, 0);
            debug_depth -= 4;
            relation_fifo.pop_back();
        }
    }


    return Ra;
}

////////////////////////////////////////////////////////////////////////////////
//
void create_node(map<char *, tg_node_t *> &task_to_node, dep_t *dep, int edge_type){
    node_t *src, *dst;

    src = dep->src;
    dst = dep->dst;

    // when the destination is the EXIT task, it's set to NULL
    if( (ENTRY == src->type) || (NULL == dst) )
        return;

    Q2J_ASSERT(src->task);
    Q2J_ASSERT(src->task->task_name);
    Q2J_ASSERT(dst->task);
    Q2J_ASSERT(dst->task->task_name);

    // See if we there is already a node in the graph for the source of this control edge.
    tg_node_t *src_nd = find_node_in_graph(src->task->task_name, task_to_node);
    if( NULL == src_nd ){
        src_nd = new tg_node_t();
        src_nd->task_name = strdup(src->task->task_name);
        task_to_node[src->task->task_name] = src_nd;
    }
    // Now that we have a graph node, add the edge to it
    tg_edge_t *new_edge = (tg_edge_t *)calloc(1, sizeof(tg_edge_t));
    new_edge->type = edge_type;
    new_edge->R = new Relation(*(dep->rel));
    tg_node_t *dst_nd = find_node_in_graph(dst->task->task_name, task_to_node);
    if( NULL == dst_nd ){
        dst_nd = new tg_node_t();
        dst_nd->task_name = strdup(dst->task->task_name);
        task_to_node[dst->task->task_name] = dst_nd;
    }
    new_edge->dst = dst_nd;
    src_nd->edges.push_back(new_edge);

    return;
}


////////////////////////////////////////////////////////////////////////////////
//
bool are_relations_equivalent(Relation *Ra, Relation *Rb){
    // If the Relations have different arrity, the are definitely not the same.
    if( (Ra->n_inp() != Rb->n_inp()) || (Ra->n_out() != Rb->n_out()) )
        return false;

    // Compute (!Ra & Rb) and see if it's satisfiable.
    bool satisf = Intersection(Complement(copy(*Ra)), copy(*Rb)).is_upper_bound_satisfiable();

    // If the Relations are equivalent, the intersection should be FALSE and thus _not_ satisfiable.
    return !satisf;
}

////////////////////////////////////////////////////////////////////////////////
//
    
void copy_task_graph_node_except_edge(tg_node_t *org_nd, map<char *, tg_node_t *> &task_to_node, dep_t *dep){
    tg_node_t *new_nd;

    // See if there is already a node in the graph for this task
    new_nd = find_node_in_graph(org_nd->task_name, task_to_node);
    if( NULL == new_nd ){
        //new_nd = (tg_node_t *)calloc(1, sizeof(tg_node_t));
        new_nd = new tg_node_t();
        new_nd->task_name = strdup(org_nd->task_name);
        task_to_node[new_nd->task_name] = new_nd;
    }

    // For every edge this node has, copy all the information into a newly created edge,
    // and if need by, create a new destinaiton task.
    for (list<tg_edge_t *>::iterator it = org_nd->edges.begin(); it != org_nd->edges.end(); it++){
        tg_edge_t *tmp_edge = *it;

        // Just being paranoid.
        Q2J_ASSERT(dep->src->task);
        Q2J_ASSERT(dep->src->task->task_name);
        Q2J_ASSERT(dep->dst->task);
        Q2J_ASSERT(dep->dst->task->task_name);

        if( !strcmp(new_nd->task_name, dep->src->task->task_name) &&
            !strcmp(tmp_edge->dst->task_name, dep->dst->task->task_name) && 
            (EDGE_ANTI == tmp_edge->type) &&
            are_relations_equivalent(tmp_edge->R, dep->rel) ) {
            continue;
        }

        tg_edge_t *new_edge = (tg_edge_t *)calloc(1, sizeof(tg_edge_t));
        new_edge->type = tmp_edge->type;
        // Copy the Relation just to be safe, since some operations on Relations are destructive.
        // However, the copying is creating a memory leak because we never delete it (as of Nov 2011).
        new_edge->R = new Relation(*(tmp_edge->R));
        tg_node_t *dst_nd = find_node_in_graph(tmp_edge->dst->task_name, task_to_node);
        if( NULL == dst_nd ){
            //dst_nd = (tg_node_t *)calloc(1, sizeof(tg_node_t));
            dst_nd = new tg_node_t();
            dst_nd->task_name = strdup(tmp_edge->dst->task_name);
            task_to_node[dst_nd->task_name] = dst_nd;
        }
        new_edge->dst = dst_nd;
        new_nd->edges.push_back(new_edge);
    }

}

void update_synch_edge_on_graph(map<char *, tg_node_t *> task_to_node, dep_t *dep, Relation fnl_rel){
    list <tg_edge_t *> new_edges;
    tg_node_t *src_task;

    // Find the task in the graph or die.
    map<char *, tg_node_t *>::iterator t_it = task_to_node.find(dep->src->task->task_name);
    assert( t_it != task_to_node.end() );
    src_task = t_it->second;

    // Get the name of the destination task of the synch edge we are trying to update.
    assert(dep->dst->task);
    assert(dep->dst->task->task_name);
    char *dst_name = dep->dst->task->task_name;

    // Traverse the list of edges that start from the source task looking for the one to update.
    for (list<tg_edge_t *>::iterator e_it = src_task->edges.begin(); e_it != src_task->edges.end(); e_it++){
        char *tmp = (*e_it)->dst->task_name;
        if( !strcmp(tmp, dst_name) ) {
            // If the destination task is correct, check the Relation.
            if( are_relations_equivalent((*e_it)->R, dep->rel) ){
                // When we find the Relation we are looking for, we update it and leave this function.

// debug starts
//printf("Changing Rel: ");
//(*e_it)->R->print_with_subs();
//printf("      To Rel: ");
//fnl_rel.print_with_subs();
// debug ends

                (*e_it)->R = new Relation(fnl_rel);
                return;
            }
        }
    }

    fprintf(stderr,"FATAL ERROR: update_synch_edge_on_graph() should never fail to find the synch edge\n");
    assert(0);
}


////////////////////////////////////////////////////////////////////////////////
//
void create_copy_of_graph_excluding_edge(map<char *, tg_node_t *> task_to_node, dep_t *dep, tg_node_t **new_source_node, tg_node_t **new_sink_node){
    map<char *, tg_node_t *> tmp_task_to_node;
    tg_node_t *new_src_nd, *new_snk_nd;
    task_t *src_task, *dst_task;

    // Make sure the dep argument contains what we think it does.
    src_task = dep->src->task;
    dst_task = dep->dst->task;
    Q2J_ASSERT(src_task);
    Q2J_ASSERT(src_task->task_name);
    Q2J_ASSERT(dst_task);
    Q2J_ASSERT(dst_task->task_name);

    // Make a copy of the graph one node at a time.
    map<char *, tg_node_t *>::iterator it;
    for(it=task_to_node.begin(); it!=task_to_node.end(); ++it){
        tg_node_t *old_nd = it->second;
        copy_task_graph_node_except_edge(old_nd, tmp_task_to_node, dep);
    }

    // Find the node that constitutes the source of this dependency in the new graph.
    new_src_nd = find_node_in_graph(src_task->task_name, tmp_task_to_node);
    Q2J_ASSERT(new_src_nd);
    new_snk_nd = find_node_in_graph(dst_task->task_name, tmp_task_to_node);
    Q2J_ASSERT(new_snk_nd);

    *new_source_node = new_src_nd;
    *new_sink_node = new_snk_nd;

    return;
}

////////////////////////////////////////////////////////////////////////////////
//
map<char *, set<dep_t *> > finalize_synch_edges(set<dep_t *> ctrl_deps, set<dep_t *> flow_deps){
    map<char *, tg_node_t *> task_to_node;
    map<char *, set<dep_t *> > resulting_map;

    // ============
    // First create a graph I_G with the different tasks (task-classes, actually) as nodes
    // and all the flow dependencies and anti-dependencies between tasks as edges.

    set<dep_t *>::iterator it;
    for (it=ctrl_deps.begin(); it!=ctrl_deps.end(); it++){
        dep_t *dep = *it;
        create_node(task_to_node, dep, EDGE_ANTI);
    }

    for (it=flow_deps.begin(); it!=flow_deps.end(); it++){
        dep_t *dep = *it;
        create_node(task_to_node, dep, EDGE_FLOW);
    }

    // ============
    // Now that we have I_G let's start the algorithm to reduce the anti-deps into a
    // set of necessary control edges.  It is yet to be determined if the algorithm
    // leads to a minimum set of control edges, for an arbitrary order of operations.


    // For every anti-edge, repeat the same steps.
    for (it=ctrl_deps.begin(); it!=ctrl_deps.end(); it++){
        set<tg_node_t *> visited_nodes;
        list<Relation *> relation_fifo;
        list<tg_node_t *> node_stack;
        Relation Rt, Ra;
        tg_node_t *source_node, *sink_node;

        dep_t *dep = *it;
        // Step 1) make a temporary copy of I_G, G, that doesn't include the
        //         anti-edge we are trying to reduce.
        create_copy_of_graph_excluding_edge(task_to_node, dep, &source_node, &sink_node);

//printf("\n>>>>>>>>>>\n   >>>>>>> Processing: %s --> %s\n",source_node->task_name, sink_node->task_name);

        // Step 2) for each pair of nodes N1,N2 in G, replace all the edges that
        //         go from N1 to N2 with their union.
        visited_nodes.clear(); // just being paranoid.
        visited_nodes.insert(source_node);
        union_graph_edges(source_node, visited_nodes);

//debug
//visited_nodes.clear(); // just being paranoid.
//visited_nodes.insert(source_node);
//dump_graph(source_node, visited_nodes);

        // Step 3) Add to every node a tautologic Relation to self:
        // {[p1,p2,...,pn] -> [p1,p2,...,pn] : TRUE}.
        visited_nodes.clear();
        visited_nodes.insert(source_node);
        add_tautologic_cycles(source_node, visited_nodes);
  
        // Step 4) Find all cycles, compute their transitive closures and union them into node.cycle
        visited_nodes.clear();
        compute_transitive_closure_of_all_cycles(source_node, visited_nodes, node_stack);
//printf("TC of cycles has been computed.\n");

        // Step 5) Find the union of the transitive edges that start at source_node and end at
        // sink_node
//printf("Computing transitive edge\n");
        visited_nodes.clear();
        relation_fifo.clear();
        Ra = find_transitive_edge(source_node, Rt, Ra, visited_nodes, relation_fifo, source_node, sink_node, 1);
        Ra.simplify();

/*
if( Ra.is_null() ){
    printf("find_transitive_edge() found no transitive edge\n");
}else{
    printf("Ra:  ");
    Ra.print_with_subs();
}
*/
     
        Relation Rsync = *(dep->rel);
        Relation Rsync_finalized;
        if(Ra.is_null()){
            Rsync_finalized = Rsync;
        }else{
//printf("Subtracting from:\n  ");
//Rsync.print_with_subs();
            Rsync_finalized = Difference(Rsync, Ra);
//printf("==> Result:\n  ");
//Rsync_finalized.print_with_subs();

//printf("Updating the task graph\n");
            update_synch_edge_on_graph(task_to_node, dep, Rsync_finalized);
//printf("\n");
        }

        if( !Rsync_finalized.is_null() && Rsync_finalized.is_upper_bound_satisfiable() ){
            dep_t *dep_finalized;
            set<dep_t *> dep_set;
            map<char *, set<dep_t *> >::iterator edge_it;

            dep_finalized = (dep_t *)calloc(1,sizeof(dep_t));
            dep_finalized->src = dep->src;
            dep_finalized->dst = dep->dst;
            dep_finalized->rel = new Relation(Rsync_finalized);
            dep_set.insert(dep_finalized);

            // See if this task already has a set of sync edges. If so, merge the old set with the new.
            edge_it = resulting_map.find(source_node->task_name);
            if( edge_it != resulting_map.end() ){
                set<dep_t *>tmp_set;
                tmp_set = resulting_map[source_node->task_name];
                dep_set.insert(tmp_set.begin(), tmp_set.end());
            }
            resulting_map[source_node->task_name] = dep_set;
        }

    }

    return resulting_map;
}


void starpu_print_types_of_formal_parameters(node_t *root){

    char *pool_decl;
    symtab_t *scope;
    symbol_t *sym;

    scope = root->symtab;
    for(sym=scope->symbols; NULL!=sym; sym=sym->next)
    {
	if( strcmp(sym->var_type, "PLASMA_desc") && strcmp(sym->var_type, "void"))
	{
	    printf("%s [type = \"%s\"]\n", sym->var_name, sym->var_type);
	}
    }
}


////////////////////////////////////////////////////////////////////////////////
//
void quark_print_types_of_formal_parameters(node_t *root){
    char *pool_decl;
    symtab_t *scope;
    symbol_t *sym;

    scope = root->symtab;
    do{
        for(sym=scope->symbols; NULL!=sym; sym=sym->next){
            if( !strcmp(sym->var_type, "PLASMA_desc") ){
                printf("desc_%s [type = \"PLASMA_desc\"]\n",sym->var_name);
                printf("data_%s [type = \"dague_ddesc_t *\"]\n",sym->var_name);
            }else{
                printf("%s [type = \"%s\"]\n",sym->var_name, sym->var_type);
            }
        }
        scope = scope->parent;
    }while(NULL != scope);

    pool_decl = create_pool_declarations();
    if( NULL != pool_decl ){
        printf("%s", pool_decl);
        free(pool_decl);
    }
    return;
}

////////////////////////////////////////////////////////////////////////////////
//
void interrogate_omega(node_t *root, var_t *head){
    var_t *var;
    und_t *und;
    map<node_t *, map<node_t *, Relation> > flow_sources, output_sources, anti_sources;
    map<char *, set<dep_t *> > outgoing_edges;
    map<char *, set<dep_t *> > incoming_edges;
    map<char *, set<dep_t *> > synch_edges;

    print_header();
#ifdef QUARK_COMPILER
    quark_print_types_of_formal_parameters(root);
#else

    for(StarPU_task_list_t *tl = starpu_task_list; tl != NULL; tl=tl->next)
    {
	tl->t->symtab = root->symtab;

    }

    starpu_print_types_of_formal_parameters(root);
#endif


    declare_global_vars(root);

    node_t *entry = DA_create_Entry();
    node_t *exit_node = DA_create_Exit();

    ////////////////////////////////////////////////////////////////////////////////
    // Find the correct set of outgoing edges using the following steps:
    // For each variable V:
    // a) create Omega relations corresponding to all FLOW dependency edges associated with V.
    // b) create Omega relations corresponding to all OUTPUT dependency edges associated with V.
    // c) based on the OUTPUT edges, kill FLOW edges that need to be killed.

    for(var=head; NULL != var; var=var->next){
	flow_sources.clear();
        output_sources.clear();
        // Create flow edges starting from the ENTRY
        flow_sources[entry]   = create_entry_relations(entry, var, DEP_FLOW);
        output_sources[entry] = create_entry_relations(entry, var, DEP_OUT);

        // For each DEF create all flow and output edges and for each USE create all anti edges.
        for(und=var->und; NULL != und ; und=und->next){

            if(is_und_write(und)){

                node_t *def = und->node;
                flow_sources[def] = create_dep_relations(und, var, DEP_FLOW, exit_node);
                output_sources[def] = create_dep_relations(und, var, DEP_OUT, exit_node);

            }
            if(is_und_read(und) && !is_phony_Entry_task(und->node->task) && !is_phony_Exit_task(und->node->task)){

                node_t *use = und->node;
                anti_sources[use] = create_dep_relations(und, var, DEP_ANTI, exit_node);

            }
        }



        // Minimize the flow dependencies (also known as true dependencies, or read-after-write)
        // by factoring in the output dependencies (also known as write-after-write).
        //

        // For every DEF that is the source of flow dependencies
        map<node_t *, map<node_t *, Relation> >::iterator flow_src_it;
        for(flow_src_it=flow_sources.begin(); flow_src_it!=flow_sources.end(); ++flow_src_it){
            char *task_name;
            set<dep_t *> dep_set;
            // Extract from the map the actual DEF from which all the deps in this map start from
            node_t *def = flow_src_it->first;
            map<node_t *, Relation> flow_deps = flow_src_it->second;

            // Keep the task name, we'll use it to insert all the edges of the task in a map later on.
            if( ENTRY == def->type ){
#ifdef DEBUG_2
                printf("\n[[ ENTRY ]] =>\n");
#endif
                task_name = strdup("ENTRY");
            }else{
                task_name = def->task->task_name;
#ifdef DEBUG_2
                printf("\n[[ %s(",def->task->task_name);
                for(int i=0; NULL != def->task->ind_vars[i]; ++i){
                    if( i ) printf(",");
                    printf("%s", def->task->ind_vars[i]);
                }
                printf(") %s ]] =>\n", tree_to_str(def));
#endif
            }

            map<node_t *, map<node_t *, Relation> >::iterator out_src_it;
            out_src_it = output_sources.find(def);
            map<node_t *, Relation> output_deps = out_src_it->second;

            // For every flow dependency that has "def" as its source
            map<node_t *, Relation>::iterator fd_it;
            for(fd_it=flow_deps.begin(); fd_it!=flow_deps.end(); ++fd_it){
                dep_t *dep = (dep_t *)calloc(1, sizeof(dep_t));

                dep->src = def;

                // Get the sink of the edge and the actual omega Relation
                node_t *sink = fd_it->first;
                Relation fd1_r = fd_it->second;

#ifdef DEBUG_2
                if( EXIT != sink->type){
                    printf("    => [[ %s(",sink->task->task_name);
                    for(int i=0; NULL != sink->task->ind_vars[i]; ++i){
                    if( i ) printf(",");
                    printf("%s", sink->task->ind_vars[i]);
                    }
                    printf(") %s ]] ", tree_to_str(sink));
                }
                fd1_r.print();
#endif

                Relation rAllKill;
                // For every output dependency that has "def" as its source
                map<node_t *, Relation>::iterator od_it;
                for(od_it=output_deps.begin(); od_it!=output_deps.end(); ++od_it){
                    Relation rKill;

                    // Check if sink of this output edge is
                    // a) the same as the source (loop carried output edge on myself)
                    // b) the source of a new flow dep that has the same sink as the original flow dep.
                    node_t *od_sink = od_it->first;
                    Relation od_r = od_it->second;

                    if( od_sink == def ){
                        // If we made it to here it means that I need to ask omega to compute:
                        // rKill := fd1_r compose od_r;  (Yes, this is the original fd)

#ifdef DEBUG_3
                        printf("Killer output dep:\n");
                        od_r.print();
#endif

                        rKill = Composition(copy(fd1_r), copy(od_r));
                        rKill.simplify();
#ifdef DEBUG_3
                        printf("Killer composed:\n");
                        rKill.print();
#endif
                    }else{

                        // See if there is a flow dep with source equal to od_sink
                        // and sink equal to "sink"
                        map<node_t *, map<node_t *, Relation> >::iterator fd2_src_it;
                        fd2_src_it = flow_sources.find(od_sink);
                        if( fd2_src_it == flow_sources.end() ){
                            continue;
                        }
                        map<node_t *, Relation> fd2_deps = fd2_src_it->second;
                        map<node_t *, Relation>::iterator fd2_it;
                        fd2_it = fd2_deps.find(sink);
                        if( fd2_it == fd2_deps.end() ){
                            continue;
                        }
                        Relation fd2_r = fd2_it->second;
                        // If we made it to here it means that I nedd to ask omega to compute:

#ifdef DEBUG_3
                        printf("Killer flow2 dep:\n");
                        fd2_r.print();
                        printf("Killer output dep:\n");
                        od_r.print();
#endif

                        rKill = Composition(copy(fd2_r), copy(od_r));
                        rKill.simplify();
#ifdef DEBUG_3
                        printf("Killer composed:\n");
                        rKill.print();
#endif
                    }
                    if( rAllKill.is_null() || rAllKill.is_set() ){
                        rAllKill = rKill;
                    }else{
                        rAllKill = Union(rAllKill, rKill);
                    }
                }
                Relation rReal;
                if( rAllKill.is_null() || rAllKill.is_set() ){
                    rReal = fd1_r;
                }
                else{
#ifdef DEBUG_3
                    printf("Final Killer:\n");
                    rAllKill.print();
#endif
                    rReal = Difference(fd1_r, rAllKill);
                }

                rReal.simplify();
#ifdef DEBUG_3
                printf("Final Edge:\n");
                rReal.print();
                printf("==============================\n");
#endif
                if( rReal.is_upper_bound_satisfiable() || rReal.is_lower_bound_satisfiable() ){

                    if( EXIT == sink->type){
#ifdef DEBUG_2
                        printf("    => [[ EXIT ]] ");
#endif
                        dep->dst = NULL;
                    }else{
                        dep->dst = sink;
#ifdef DEBUG_2
                        printf("    => [[ %s(",sink->task->task_name);
                        for(int i=0; NULL != sink->task->ind_vars[i]; ++i){
                            if( i ) printf(",");
                            printf("%s", sink->task->ind_vars[i]);
                        }
                        printf(") %s ]] ", tree_to_str(sink));
#endif
                    }
#ifdef DEBUG_2
                    rReal.print_with_subs(stdout);
#endif
                    dep->rel = new Relation(rReal);
                    dep_set.insert(dep);

                }
            }

            map<char *, set<dep_t *> >::iterator edge_it;
            edge_it = outgoing_edges.find(task_name);
            if( edge_it != outgoing_edges.end() ){
                set<dep_t *>tmp_set;
                tmp_set = outgoing_edges[task_name];
                dep_set.insert(tmp_set.begin(), tmp_set.end());
            }
            outgoing_edges[task_name] = dep_set;
        }
    }

#ifdef DEBUG_2
printf("================================================================================\n");
#endif

    // Minimize the anti dependencies (also known as write-after-read).
    // by factoring in the flow dependencies (also known as read-after-write).
    //

//printf("\n-------------------------------------------------\n");

    // For every USE that is the source of anti dependencies
    map<node_t *, map<node_t *, Relation> >::iterator anti_src_it;
    for(anti_src_it=anti_sources.begin(); anti_src_it!=anti_sources.end(); ++anti_src_it){
        Relation Ra0, Roi, Rai;
        set<dep_t *> dep_set;
        // Extract from the map the actual USE from which all the deps in this map start from
        node_t *use = anti_src_it->first;
        map<node_t *, Relation> anti_deps = anti_src_it->second;

        // Iterate over all anti-dependency edges (but skip the self-edge).
        map<node_t *, Relation>::iterator ad_it;
        for(ad_it=anti_deps.begin(); ad_it!=anti_deps.end(); ++ad_it){
            // Get the sink of the edge.
            node_t *sink = ad_it->first;
            // Skip self-edges that cannot be carried by loops.
            if( sink == use ){
                // TODO: If the induction variables of ALL loops the enclose this use/def 
                // TODO: appear in the indices of the matrix reference, then this cannot
                // TODO: be a loop carried dependency.  If there is at least one enclosing
                // TODO: loop for which this matrix reference is loop invariant, then we
                // TODO: should keep the anti-edge.  Keeping it anyway is safe, but
                // TODO: puts unnecessary burden on Omega when calculating the transitive.
                // TODO: relations and the transitive closures of the loops.
                ;
            }
            Rai = ad_it->second;

            dep_t *dep = (dep_t *)calloc(1, sizeof(dep_t));
            dep->src = use;
            dep->dst = sink;
            dep->rel = new Relation(Rai);
            dep_set.insert(dep);

//printf("Inserting anti-dep %s::%s -> %s::%s\n",use->task->task_name, tree_to_str(use), sink->task->task_name, tree_to_str(sink));

        }

        // see if the current task already has some synch edges and if so merge them with the new ones.
        map<char *, set<dep_t *> >::iterator edge_it;
        char *task_name = use->task->task_name;
        edge_it = synch_edges.find(task_name);
        if( edge_it != synch_edges.end() ){
            set<dep_t *>tmp_set;
            tmp_set = synch_edges[task_name];
            dep_set.insert(tmp_set.begin(), tmp_set.end());
        }
        synch_edges[task_name] = dep_set;
    }


    set<dep_t *> ctrl_deps = edge_map_to_dep_set(synch_edges);
    set<dep_t *> flow_deps = edge_map_to_dep_set(outgoing_edges);
    synch_edges = finalize_synch_edges(ctrl_deps, flow_deps);

    #ifdef DEBUG_2
    printf("================================================================================\n");
    #endif

    //////////////////////////////////////////////////////////////////////////////////////////
    // inverse all outgoing edges (unless they are going to the EXIT) to generate the incoming
    // edges in the JDF.
    map<char *, set<dep_t *> >::iterator edge_it;
    edge_it = outgoing_edges.begin();
    for( ;edge_it != outgoing_edges.end(); ++edge_it ){

	char *task_name;
	set<dep_t *> outgoing_deps;

	outgoing_deps = edge_it->second;

	if( outgoing_deps.empty() ){
	    continue;
	}

#ifdef DEBUG_2
	task_t *src_task = (*outgoing_deps.begin())->src->task;

	if( NULL == src_task )
	    printf("ENTRY \n");
	else
	    printf("%s \n",src_task->task_name);
#endif

	set<dep_t *>::iterator it;
	for (it=outgoing_deps.begin(); it!=outgoing_deps.end(); it++){
	    set<dep_t *>incoming_deps;
	    dep_t *dep = *it;

	    // If the destination is the EXIT, we do not inverse the edge
	    if( NULL == dep->dst ){
		continue;
	    }

	    node_t *sink = dep->dst;
	    task_name = sink->task->task_name;

	    // Create the incoming edge by inverting the outgoing one.
	    dep_t *new_dep = (dep_t *)calloc(1, sizeof(dep_t));
	    Relation inv = *dep->rel;
	    new_dep->rel = new Relation(Inverse(copy(inv)));
	    new_dep->src = dep->src;
	    new_dep->dst = dep->dst;

	    // If there are some deps already associated with this task, retrieve them
	    map<char *, set<dep_t *> >::iterator edge_it;
	    edge_it = incoming_edges.find(task_name);
	    if( edge_it != incoming_edges.end() ){
		incoming_deps = incoming_edges[task_name];
	    }
	    // Add the new dep to the list of deps we will associate with this task
	    incoming_deps.insert(new_dep);
	    // Associate the deps with the task
	    incoming_edges[task_name] = incoming_deps;
	}

    }

#ifdef DEBUG_2
printf("================================================================================\n");
#endif

    //////////////////////////////////////////////////////////////////////////////////////////
    // Print all edges.

    edge_it = outgoing_edges.begin();
    for( ;edge_it != outgoing_edges.end(); ++edge_it ){
	task_t *src_task;
	char *task_name;
	set<dep_t *> deps;

	task_name = edge_it->first;
	deps = edge_it->second;

	// Get the source task from the dependencies
	if( !deps.empty() ){
	    src_task = (*deps.begin())->src->task;
	}else{
	    // If there are no outgoing deps, get the source task from the incoming dependencies
	    set<dep_t *> in_deps = incoming_edges[task_name];
	    if( !in_deps.empty() ){
		src_task = (*in_deps.begin())->src->task;
	    }else{
		// If there are no incoming and no outgoing deps, skip this task
		continue;
	    }
	}

	// If the source task is NOT the ENTRY, then dump all the info
	if( NULL != src_task ){

	    printf("\n\n%s(",task_name);
	    for(int i=0; NULL != src_task->ind_vars[i]; ++i){
		if( i ) printf(",");
		printf("%s", src_task->ind_vars[i]);
	    }
	    printf(")\n");

	    Relation S_es = process_and_print_execution_space(src_task->task_node);
	    printf("\n");

	    node_t *reference_data_element = starpu_print_default_task_placement(src_task->task_node);
//	    node_t *reference_data_element = print_default_task_placement(src_task->task_node);
	    printf("\n");

	    print_pseudo_variables(deps, incoming_edges[task_name]);

	    list <char *>ptask_list = print_edges_and_create_pseudotasks(deps, incoming_edges[task_name], S_es, reference_data_element);
	    S_es.Null();
	    printf("\n");

	    printf("  /*\n  Anti-dependencies:\n");

	    // If this task has no name, then it's probably a phony task, so ignore it.
	    if( (NULL != src_task->task_name) ){
                map<char *, set<dep_t *> >::iterator synch_edge_it;

                for( synch_edge_it = synch_edges.begin(); synch_edge_it!= synch_edges.end(); ++synch_edge_it){
                    char *tmp_task_name = synch_edge_it->first;
                    if( strcmp(tmp_task_name, src_task->task_name ) )
                               continue;
                    set<dep_t *> synch_dep_set = synch_edge_it->second;
                    set<dep_t *>::iterator synch_dep_it;

                    // Traverse all the entries of the set stored in synch_edges[ this task's name ] and print them
                    for(synch_dep_it=synch_dep_set.begin(); synch_dep_it!=synch_dep_set.end(); ++synch_dep_it){
                        node_t *use = (*synch_dep_it)->src;
                        assert(use->task == src_task);
                        node_t *sink = (*synch_dep_it)->dst;
                        Relation ad_r = *((*synch_dep_it)->rel);
                        char *n1 = use->task->task_name;
                        char *n2 = sink->task->task_name;
                        printf("  ANTI edge from %s:%s to %s:%s ",n1, tree_to_str(use), n2, tree_to_str(sink));
                        ad_r.print_with_subs();
                    }
                }
            }else{
                printf("DEBUG: unnamed task.\n");
            }

            printf("\n");

            printf("  */\n\n");

            print_body(src_task);

            // Print all the pseudo-tasks that were created by print_edges_and_create_pseudotasks()
            list <char *>::iterator ptask_it;
            for(ptask_it=ptask_list.begin(); ptask_it!=ptask_list.end(); ++ptask_it){
                printf("\n/*\n * Pseudo-task\n */\n%s\n",*ptask_it);
            }
        }
    }
}

void add_colocated_data_info(char *a, char *b){
    Q2J_ASSERT( (NULL != a) && (NULL != b) );
    q2j_colocated_map[string(a)] = string(b);
}


void store_global_invariant(node_t *invar_expr){
    q2j_global_invariants.insert(invar_expr);
}

static const char *econd_tree_to_ub(node_t *econd){
    stringstream ss;
    char *a, *b;
        
    switch( econd->type ){
        case L_AND:
            a = strdup( econd_tree_to_ub(econd->u.kids.kids[0]) );
            b = strdup( econd_tree_to_ub(econd->u.kids.kids[1]) );
            ss << "( (" << a << " < " << b << ")? " << a << " : " << b << " )";
            free(a);
            free(b);
            return strdup(ss.str().c_str());
// TODO: handle logical or (L_OR) as well.

        case LE:
            ss << tree_to_str(DA_rel_rhs(econd));
            return strdup(ss.str().c_str());

        case LT:
            ss << tree_to_str(DA_rel_rhs(econd)) << "-1";
            return strdup(ss.str().c_str());

        default:
            fprintf(stderr,"ERROR: econd_tree_to_ub() cannot deal with node of type: %d\n",econd->type);
            exit(-1);
    }
}

static Relation process_and_print_execution_space(node_t *node){
    int i;
    node_t *tmp;
    list<node_t *> params;
    map<string, Variable_ID> vars;
    Relation S;

    printf("  /* Execution space */\n");
    for(tmp=node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        params.push_front(tmp);
    }
    Q2J_ASSERT( !params.empty() );

    S = Relation(params.size());
    F_And *S_root = S.add_and();

    for(i=1; !params.empty(); i++ ) {
        char *var_name;
        tmp = params.front();

        var_name = DA_var_name(DA_loop_induction_variable(tmp));
        S.name_set_var( i, var_name );
        vars[var_name] = S.set_var( i );

        params.pop_front();
    }


    // Bound all induction variables of the loops enclosing the USE
    // using the loop bounds. Also demand that "LB <= UB" (or "LB < UB")
    i=1;
    for(tmp=node->enclosing_loop; NULL != tmp; tmp=tmp->enclosing_loop ){
        char *var_name = DA_var_name(DA_loop_induction_variable(tmp));

        // Form the Omega expression for the lower bound
        Variable_ID var = vars[var_name];

        GEQ_Handle imin = S_root->add_GEQ();
        imin.update_coef(var,1);
        expr_to_Omega_coef(DA_loop_lb(tmp), imin, -1, vars, S);

        // Form the Omega expression for the upper bound
        process_end_condition(DA_for_econd(tmp), S_root, vars, NULL, S);

        GEQ_Handle lb_le_ub = S_root->add_GEQ();
        process_end_condition(DA_for_econd(tmp), S_root, vars, DA_loop_lb(tmp), S);
        
    }

    // Ask Omega to simplify the Relation for us.
    S.simplify();
    (void)S.print_with_subs_to_string(false);

    // Print the execution space based on the bounds that exist in the relation.
    set<const char *> prev_vars;
    for(i=1; i<=S.n_set(); i++){
        const char *var_name = strdup(S.set_var(i)->char_name());

        printf("  %s = ", var_name);
        expr_t *solution = solveExpressionTreeForVar(relation_to_tree(S), var_name, S);
        if( NULL != solution )
            printf("%s\n", expr_tree_to_str(solution));
        else
            printf("%s\n", find_bounds_of_var(relation_to_tree(S), var_name, prev_vars, S));
        prev_vars.insert(var_name);
    }

    // Do some memory clean-up
    while(!prev_vars.empty()){
        free((void *)*prev_vars.begin());
        prev_vars.erase(prev_vars.begin());
    }

    return S;
}

////////////////////////////////////////////////////////////////////////////////
//
static void print_pseudo_variables(set<dep_t *>out_deps, set<dep_t *>in_deps){
    set<dep_t *>::iterator it;
    map<string, string> pseudo_vars;

   // Create a mapping between pseudo_variables and actual variables

   // OUTGOING
    for (it=out_deps.begin(); it!=out_deps.end(); it++){
       dep_t *dep = *it;
       // WARNING: This is needed by Omega. If you remove it you get strange
       // assert() calls being triggered inside the Omega library.
       (void)(*dep->rel).print_with_subs_to_string(false);

       if( NULL != dep->src->task ){
           pseudo_vars[dep->src->var_symname] = tree_to_str(dep->src);
       }
       if( NULL != dep->dst ){
           pseudo_vars[dep->dst->var_symname] = tree_to_str(dep->dst);
       }
   }

   // INCOMING
   for (it=in_deps.begin(); it!=in_deps.end(); it++){
       dep_t *dep = *it;
       // WARNING: This is needed by Omega. If you remove it you get strange
       // assert() calls being triggered inside the Omega library.
       (void)(*dep->rel).print_with_subs_to_string(false);

       Q2J_ASSERT( NULL != dep->dst);
       pseudo_vars[dep->dst->var_symname] = tree_to_str(dep->dst);

       if( NULL != dep->src->task ){
           pseudo_vars[dep->src->var_symname] = tree_to_str(dep->src);
       }
   }

   // Dump the map.
   map<string, string>::iterator pvit;
   for (pvit=pseudo_vars.begin(); pvit!=pseudo_vars.end(); pvit++){
       /*
        * JDF & QUARK specific optimization:
        * Add the keyword "data_" infront of the matrix to
        * differentiate the matrix from the struct.
        */
#ifdef QUARK_COMPILER
       printf("  /* %s == data_%s */\n",(pvit->first).c_str(), (pvit->second).c_str());
#else
       printf("  /* %s == %s */\n",(pvit->first).c_str(), (pvit->second).c_str());
#endif // QUARK_COMPILER
   }

}

// We are assuming that all leaves will be kids of a MUL or a DIV, or they will be an INTCONSTANT
// Conversely we are assuming that all MUL and DIV nodes will have ONLY leaves as kids.
// Leaves BTW are the types INTCONSTANT and IDENTIFIER.
static void groupExpressionBasedOnSign(expr_t *exp, set<expr_t *> &pos, set<expr_t *> &neg){

    if( NULL == exp )
        return;

    switch( exp->type ){
        case INTCONSTANT:
            if( exp->value.int_const < 0 ){
                neg.insert(exp);
            }else{
                pos.insert(exp);
            }
            break;

        case IDENTIFIER:
            pos.insert(exp);
            break;

        case MUL:
        case DIV:
            if( ( (INTCONSTANT != exp->l->type) && (INTCONSTANT != exp->r->type) ) ||
                ( (IDENTIFIER != exp->l->type) && (IDENTIFIER != exp->r->type) )      ){
               fprintf(stderr,"ERROR: groupExpressionBasedOnSign() cannot handle MUL and/or DIV nodes with kids that are not leaves: \"%s\"\n",expr_tree_to_str(exp));
                exit(-1);
            }

            if( INTCONSTANT == exp->l->type ){
                if( exp->l->value.int_const < 0 ){
                    neg.insert(exp);
                }else{
                    pos.insert(exp);
                }
            }else{
                if( exp->r->value.int_const < 0 ){
                    neg.insert(exp);
                }else{
                    pos.insert(exp);
                }
            }
            break;

        default:
            groupExpressionBasedOnSign(exp->l, pos, neg);
            groupExpressionBasedOnSign(exp->r, pos, neg);
            break;
    }
    return;
}

const char *type_to_str(int type){

    switch(type){
        case EMPTY: return "EMPTY";
        case INTCONSTANT: return "INTCONSTANT";
        case IDENTIFIER: return "IDENTIFIER";
        case ADDR_OF: return "ADDR_OF";
        case STAR: return "STAR";
        case PLUS: return "PLUS";
        case MINUS: return "MINUS";
        case TILDA: return "TILDA";
        case BANG: return "BANG";
        case ASSIGN: return "ASSIGN";
        case COND: return "COND";
        case ARRAY: return "ARRAY";
        case FCALL: return "FCALL";
        case ENTRY: return "ENTRY";
        case EXIT: return "EXIT";
        case EXPR: return "EXPR";
        case ADD: return "ADD";
        case SUB: return "SUB";
        case MUL: return "MUL";
        case DIV: return "DIV";
        case MOD: return "MOD";
        case B_AND: return "B_AND";
        case B_XOR: return "B_XOR";
        case B_OR: return "B_OR";
        case LSHIFT: return "LSHIFT";
        case RSHIFT: return "RSHIFT";
        case LT: return "LT";
        case GT: return "GT";
        case LE: return "LE";
        case GE: return "GE";
        case DEREF: return "DEREF";
        case S_U_MEMBER: return "S_U_MEMBER";
        case COMMA_EXPR: return "COMMA_EXPR";
        case BLOCK: return "BLOCK";
        default: return "???";
    }
}


static bool is_expr_simple(const expr_t *exp){
    switch( exp->type ){
        case IDENTIFIER:
            return true;

        case INTCONSTANT:
            return true;
    }
    return false;
}

const char *expr_tree_to_str(const expr_t *exp){
    string str;
    str = _expr_tree_to_str(exp);
    return strdup(str.c_str());
}

static string _expr_tree_to_str(const expr_t *exp){
    stringstream ss, ssL, ssR;
    unsigned int skipSymbol=0, first=1;
    unsigned int r_needs_paren = 0, l_needs_paren = 0;
    size_t j;
    set<expr_t *> pos, neg;
    set<expr_t *>::iterator it;
    string str;

    if( NULL == exp )
        return "";

    switch( exp->type ){
        case IDENTIFIER:
            str = string(exp->value.name);
            return str;

        case INTCONSTANT:
            if( exp->value.int_const < 0 )
                ss << "(" << exp->value.int_const << ")";
            else
                ss << exp->value.int_const;
            return ss.str();

        case EQ_OP:
        case GE:

            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            groupExpressionBasedOnSign(exp->l, pos, neg);
            groupExpressionBasedOnSign(exp->r, neg, pos);

            // print all the positive members on the left
            first = 1;
            for(it=pos.begin(); it != pos.end(); it++){
                expr_t *e = *it;

                if( INTCONSTANT == e->type ){
                    // Only print the constant if it's non-zero, or if it's the only thing on this side
                    if( (0 != e->value.int_const) || (1 == pos.size()) ){
                        if( !first ){
                            ssL << "+";
                            l_needs_paren = 1;
                        }
                        first = 0;
                        ssL << labs(e->value.int_const);
                    }
                }else{
                    if( !first ){
                        ssL << "+";
                        l_needs_paren = 1;
                    }
                    first = 0;
                    if( INTCONSTANT == e->l->type ){
                        // skip the "1*"
                        if( MUL != e->type || labs(e->l->value.int_const) != 1 ){
                            ssL << labs(e->l->value.int_const);
                            ssL << type_to_symbol(e->type);
                        }
                        ssL << _expr_tree_to_str(e->r);
                    }else{
                        // in this case we can skip the "one" even for divisions
                        if( labs(e->r->value.int_const) != 1 ){
                            ssL << labs(e->r->value.int_const);
                            ssL << type_to_symbol(e->type);
                        }
                        ssL << _expr_tree_to_str(e->l);
                    }
                }
            }
            if( 0 == pos.size() ){
                ssL << "0";
            }

            // print all the negative members on the right
            first = 1;
            for(it=neg.begin(); it != neg.end(); it++){
                expr_t *e = *it;
                if( INTCONSTANT == e->type ){
                    // Only print the constant if it's non-zero, or if it's the only thing on this side
                    if( (0 != e->value.int_const) || (1 == neg.size()) ){
                        if( !first ){
                            ssR << "+";
                            r_needs_paren = 1;
                        }
                        first = 0;
                        ssR << labs(e->value.int_const);
                    }
                }else{
                    if( !first ){
                        ssR << "+";
                        r_needs_paren = 1;
                    }
                    first = 0;
                    if( INTCONSTANT == e->l->type ){
                        if( MUL != e->type || labs(e->l->value.int_const) != 1 ){
                            ssR << labs(e->l->value.int_const);
                            ssR << type_to_symbol(e->type);
                        }
                        ssR << _expr_tree_to_str(e->r);
                    }else{
                        if( labs(e->r->value.int_const) != 1 ){
                            ssR << labs(e->r->value.int_const);
                            ssR << type_to_symbol(e->type);
                        }
                        ssR << _expr_tree_to_str(e->l);
                    }
                }
            }
            if( 0 == neg.size() ){
                ssR << "0";
            }

            // Add some parentheses to make it easier for parser that will read in the JDF.
            ss << "(";
            if( l_needs_paren )
                ss << "(" << ssL.str() << ")";
            else
                ss << ssL.str();

            ss << type_to_symbol(exp->type);

            if( r_needs_paren )
                ss << "(" << ssR.str() << "))";
            else
                ss << ssR.str() << ")";

            return ss.str();

        case L_AND:
        case L_OR:

            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            // If one of the two sides is a tautology, we will get a NULL string
            // and we have no reason to print the "&&" or "||" sign.
            if( _expr_tree_to_str(exp->l).empty() )
                return _expr_tree_to_str(exp->r);
            if( _expr_tree_to_str(exp->r).empty() )
                return _expr_tree_to_str(exp->l);

            if( L_OR == exp->type )
                ss << "(";
            ss << _expr_tree_to_str(exp->l);
            ss << " " << type_to_symbol(exp->type) << " ";
            ss << _expr_tree_to_str(exp->r);
            if( L_OR == exp->type )
                ss << ")";
            return ss.str();

        case ADD:
            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            if( (NULL != exp->l) && (INTCONSTANT == exp->l->type) ){
                if( exp->l->value.int_const != 0 ){
                    ss << _expr_tree_to_str(exp->l);
                }else{
                    skipSymbol = 1;
                }
            }else{
                ss << _expr_tree_to_str(exp->l);
            }


            // If the thing we add is a "-c" (where c is a constant)
            // detect it so we print "-c" instead of "+(-c)"
            if( (NULL != exp->r) && (INTCONSTANT == exp->r->type) && (exp->r->value.int_const < 0) ){
                ss << "-" << labs(exp->r->value.int_const);
                return ss.str();
            }

            if( !skipSymbol )
                ss << type_to_symbol(ADD);

            ss << _expr_tree_to_str(exp->r);

            return ss.str();

        case MUL:
            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            if( INTCONSTANT == exp->l->type ){
                if( exp->l->value.int_const != 1 ){
                    ss << "(";
                    ss << _expr_tree_to_str(exp->l);
                }else{
                    skipSymbol = 1;
                }
            }else{
                ss << _expr_tree_to_str(exp->l);
            }

            if( !skipSymbol )
                ss << type_to_symbol(MUL);

            ss << _expr_tree_to_str(exp->r);

            if( !skipSymbol )
                ss << ")";

            return ss.str();

        case DIV:
            Q2J_ASSERT( (NULL != exp->l) && (NULL != exp->r) );

            ss << "(";
            if( INTCONSTANT == exp->l->type ){
                if( exp->l->value.int_const < 0 ){
                    ss << "(" << _expr_tree_to_str(exp->l) << ")";
                }else{
                    ss << _expr_tree_to_str(exp->l);
                }
            }else{
                ss << _expr_tree_to_str(exp->l);
            }

            ss << type_to_symbol(DIV);

            if( (INTCONSTANT == exp->r->type) && (exp->r->value.int_const > 0) )
                ss << _expr_tree_to_str(exp->r);
            else
                ss << "(" << _expr_tree_to_str(exp->r) << ")";

            ss << ")";
            return ss.str();

        default:
            ss << "{" << exp->type << "}";
            return ss.str();
    }
    return string();
}

////////////////////////////////////////////////////////////////////////////////
//
static char *create_pseudotask(task_t *parent_task, Relation S_es, Relation cond, node_t *data_element, char *var_pseudoname, int ptask_count, const char *inout){
    int var_count;
    char *parent_task_name, *pseudotask_name;
    char *formal_parameters = NULL, *parent_formal_parameters = NULL;
    char *ptask_str = NULL, *mtrx_name;
    Relation newS_es;
    set <const char *> prev_vars;
    str_pair_t *solved_vars;

    mtrx_name = tree_to_str(DA_array_base(data_element));

    if( !cond.is_null() ){
        newS_es = Intersection(copy(S_es), Domain(copy(cond)));
        newS_es.simplify();
    }else{
        newS_es = copy(S_es);
    }

    // Find the maximum number of variable substitutions we might need and add one for the termination flag.
    for(var_count=0; NULL != parent_task->ind_vars[var_count]; ++var_count)
        /* nothing */;
    solved_vars = (str_pair_t *)calloc(var_count+1, sizeof(str_pair_t));
    var_count = 0;

    // We start with the parameters in order to discover which ones should be included.
    for(int i=0; NULL != parent_task->ind_vars[i]; ++i){
        const char *var_name = parent_task->ind_vars[i];
        expr_t *solution = solveExpressionTreeForVar(relation_to_tree(newS_es), var_name, copy(newS_es));
        // If there is a solution it means that this parameter has a fixed value and not a range.
        // That means that there is no point in including it as a parameter of the pseudo-task.
        if( NULL != solution ){
            const char *solution_str = expr_tree_to_str(solution);
            solved_vars[var_count].str1 = var_name;
            solved_vars[var_count].str2 = solution_str;
            var_count++;

            if( NULL != parent_formal_parameters )
                parent_formal_parameters = append_to_string(parent_formal_parameters, ",", NULL, 0);
            parent_formal_parameters = append_to_string(parent_formal_parameters, solution_str, NULL, 0);
        }else{
            ptask_str = append_to_string(ptask_str, var_name, "  %s = ", 5+strlen(var_name)); 
            ptask_str = append_to_string(ptask_str, find_bounds_of_var(relation_to_tree(newS_es), var_name, prev_vars, copy(newS_es)), NULL, 0);

            // the following code is for generating the string for the caller (the real task)
            if( NULL != formal_parameters )
                formal_parameters = append_to_string(formal_parameters, ",", NULL, 0);
            formal_parameters = append_to_string(formal_parameters, var_name, NULL, 0);
            ptask_str = append_to_string(ptask_str, "\n", NULL, 0);

            if( NULL != parent_formal_parameters )
                parent_formal_parameters = append_to_string(parent_formal_parameters, ",", NULL, 0);
            parent_formal_parameters = append_to_string(parent_formal_parameters, var_name, NULL, 0);

            prev_vars.insert(var_name);
        }
    }

    // Delete the "previous variables" set, to clean up some memory
    while(!prev_vars.empty()){
        prev_vars.erase(prev_vars.begin());
    }

    // Now that we know which parameters define the execution space of this pseudo-task, we can print the pseudo-task
    // in the body of the real task, and complete the "header" of the pseudo-task string.

    // create_pseudotask() was called by print_edges_and_create_pseudotasks(), so if we print here
    // the string will end up at the right place in the body of the real task.
    asprintf( &pseudotask_name , "xxxxxx%s_%s_data_%s%d(%s)",parent_task->task_name, inout, mtrx_name, ptask_count, formal_parameters);

    printf("y%s y%s\n",var_pseudoname, pseudotask_name);

    // Put the name and parameters of the pseudo-task in the beginning of the string.
    ptask_str = append_to_string(pseudotask_name, ptask_str, "\n%s", 1+strlen(ptask_str)); 

#if 0
    // This code generates a predicate expressed as a range, to ensure that tasks are not generated if they are impossible.
    // This functionality does not seem to be necessary any more, so we removed it.
    if( !cond.is_null() ){
        Relation dom = Relation(Domain(cond));
        // WARNING: This is needed by Omega. If you remove it you get strange
        // assert() calls being triggered inside the Omega library.
        (void)dom.print_with_subs_to_string(false);
        const char *tmp = expr_tree_to_str(relation_to_tree(dom)); 
        ptask_str = append_to_string(ptask_str, tmp, "  /*_dague_pseudotask_predicate = 1..%s*/\n", 40+strlen(tmp));
    }
#endif

    asprintf( &parent_task_name , "%s(%s)",parent_task->task_name, parent_formal_parameters);

    //char *data_str = tree_to_str(data_element);
    char *data_str = tree_to_str_with_substitutions(data_element, solved_vars);
#ifdef QUARK_COMPILER
    ptask_str = append_to_string(ptask_str, data_str, "\n  : data_%s\n\n",12+strlen(data_str));
#else
    ptask_str = append_to_string(ptask_str, data_str, "\n  : %s\n\n",7+strlen(data_str));
#endif
    if( !strcmp(inout,"in") ){
        ptask_str = append_to_string(ptask_str, var_pseudoname, "  RW %s",5+strlen(var_pseudoname));
#ifdef QUARK_COMPILER
        ptask_str = append_to_string(ptask_str, data_str, " <- data_%s\n",11+strlen(data_str));
#else
	ptask_str = append_to_string(ptask_str, data_str, " <- %s\n",6+strlen(data_str));
#endif
	ptask_str = append_to_string(ptask_str, var_pseudoname, "       -> %s",10+strlen(var_pseudoname));
        ptask_str = append_to_string(ptask_str, parent_task_name, " %s\n",2+strlen(parent_task_name));
    }else{
        ptask_str = append_to_string(ptask_str, var_pseudoname, "  RW %s",5+strlen(var_pseudoname));
        ptask_str = append_to_string(ptask_str, var_pseudoname, " <- %s", 4+strlen(var_pseudoname));
        ptask_str = append_to_string(ptask_str, parent_task_name, " %s\n",2+strlen(parent_task_name));

#ifdef QUARK_COMPILER
        ptask_str = append_to_string(ptask_str, data_str, "        -> data_%s\n",17+strlen(data_str));
#else
	ptask_str = append_to_string(ptask_str, data_str, "        -> %s\n",12+strlen(data_str));
#endif 
    }
    ptask_str = append_to_string(ptask_str,"BODY\n/* nothing */\nEND\n", NULL, 0);

    free(parent_task_name);
    //free(pseudotask_name);
    free(mtrx_name);

    return ptask_str;
}


bool need_pseudotask(node_t *ref1, node_t *ref2){
    bool need_ptask = false;
    char *comm_mtrx, *refr_mtrx;

    if( _q2j_produce_shmem_jdf )
        return false;

    comm_mtrx = tree_to_str(DA_array_base(ref1));
    refr_mtrx = tree_to_str(DA_array_base(ref2));

    // If the matrices are different and not co-located, we need a pseudo-task.
    if( strcmp(comm_mtrx,refr_mtrx) && ( q2j_colocated_map.find(comm_mtrx) == q2j_colocated_map.end() || q2j_colocated_map.find(refr_mtrx) == q2j_colocated_map.end() || q2j_colocated_map[comm_mtrx].compare(q2j_colocated_map[refr_mtrx]) ) ){
        need_ptask = true;
    }else{
        // If the element we are communicating is not the same as the reference element, we also need a pseudo-task.
        int count = DA_array_dim_count(ref1);
        if( DA_array_dim_count(ref2) != count ){
            fprintf(stderr,"Matrices with different dimension counts detected \"%s\" and \"%s\"."
                           " This should never happen in DPLASMA\n",
                           tree_to_str(ref1), tree_to_str(ref2));
            need_ptask = true;
        }

        for(int i=0; i<count && !need_ptask; i++){
            char *a = tree_to_str(DA_array_index(ref1, i));
            char *b = tree_to_str(DA_array_index(ref2, i));
            if( strcmp(a,b) )
                need_ptask = true;
            free(a);
            free(b);
        }
    }
    free(comm_mtrx);
    free(refr_mtrx);

    return need_ptask;
}


////////////////////////////////////////////////////////////////////////////////
//
list<char *> print_edges_and_create_pseudotasks(set<dep_t *>outg_deps, set<dep_t *>incm_deps, Relation S_es, node_t *reference_data_element){
    int pseudotask_count = 0;
    task_t *this_task;
    set<dep_t *>::iterator dep_it;
    set<char *> vars;
    map<char *, set<dep_t *> > incm_map, outg_map;
    list<char *> ptask_list;

    if( outg_deps.empty() && incm_deps.empty() ){
        return ptask_list;
    }

    this_task = (*outg_deps.begin())->src->task;


    // Group the edges based on the variable they flow into or from
    for (dep_it=incm_deps.begin(); dep_it!=incm_deps.end(); dep_it++){
           char *symname = (*dep_it)->dst->var_symname;
           incm_map[symname].insert(*dep_it);
           vars.insert(symname);
    }
    for (dep_it=outg_deps.begin(); dep_it!=outg_deps.end(); dep_it++){
           char *symname = (*dep_it)->src->var_symname;
           outg_map[symname].insert(*dep_it);
           vars.insert(symname);
    }

    if( vars.size() > MAX_PARAM_COUNT ){
        fprintf(stderr,"WARNING: Number of variables (%lu) exceeds %d\n", vars.size(), MAX_PARAM_COUNT);
    }


    // For each variable print all the incoming and the outgoing edges
    set<char *>::iterator var_it;
    for (var_it=vars.begin(); var_it!=vars.end(); var_it++){
        bool insert_fake_read = false;
        char *var_pseudoname = *var_it;
        set<dep_t *>ideps = incm_map[var_pseudoname];
        set<dep_t *>odeps = outg_map[var_pseudoname];

        if( !ideps.empty() && !odeps.empty() ){
            printf("  RW    ");
        }else if( !ideps.empty() ){
            printf("  READ  ");
        }else if( !odeps.empty() ){
            /* 
             * DAGuE does not like write-only variables, so make it RW and make
             * it read from the data matrix tile that corresponds to this variable.
             */
            printf("  RW    ");
            insert_fake_read = true;
        }else{
            Q2J_ASSERT(0);
        }

        if( ideps.size() > MAX_PRED_COUNT )
            fprintf(stderr,"WARNING: Number of incoming edges (%lu) for variable \"%s\" exceeds %d\n", ideps.size(), var_pseudoname, MAX_PRED_COUNT);
        if( odeps.size() > MAX_PRED_COUNT )
            fprintf(stderr,"WARNING: Number of outgoing edges (%lu) for variable \"%s\" exceeds %d\n", odeps.size(), var_pseudoname, MAX_PRED_COUNT);

        // Print the pseudoname
        printf("%s",var_pseudoname);

        // print the incoming edges
        for (dep_it=ideps.begin(); dep_it!=ideps.end(); dep_it++){
             dep_t *dep = *dep_it;
             list< pair<expr_t *,Relation> > cond_list;
             list< pair<expr_t *, Relation> >::iterator cond_it, cond_it2;
             string cond;

             // Needed by Omega
             (void)(*dep->rel).print_with_subs_to_string(false);

             Q2J_ASSERT( NULL != dep->dst);

             // If the condition has disjunctions (logical OR operators) then split them so that each one
             // is treated independently.
             cond_list = simplify_conditions_and_split_disjunctions(*dep->rel, S_es);
             for(cond_it = cond_list.begin(); cond_it != cond_list.end(); cond_it++){
                 string cond = expr_tree_to_str(cond_it->first);
                 bool printed_condition = false;

                 if ( dep_it!=ideps.begin() || cond_it != cond_list.begin() )
                     printf("         ");
                 printf(" <- ");

                 task_t *src_task = dep->src->task;

                 
                 // TODO: If the conditions are mutually exclusive, we do not need to do the following step.

                 // For each condition that resulted from spliting a disjunction, negate
                 // all the other parts of the disjunction
                 for(cond_it2 = cond_list.begin(); cond_it2 != cond_it; cond_it2++){
                     string cond2 = expr_tree_to_str(cond_it2->first);
                     if( !cond2.empty() ){
                         if( printed_condition )
                             printf(" & ");
                         printf("(!(%s))",cond2.c_str());
                         printed_condition = true;
                     }
                 }
                 if( !cond.empty() ){
                     if( printed_condition )
                         printf(" & ");
                     printf("%s ? ",cond.c_str());
                 }else if( printed_condition ){
                     printf(" ? ");
                 }

                 if( NULL != src_task ){
                     printf("%s ", dep->src->var_symname);
                     printf("%s(",src_task->task_name);
                     print_actual_parameters(dep, relation_to_tree(cond_it->second), SOURCE);
                     printf(") ");
                 }else{ // ENTRY
                     if( need_pseudotask(dep->dst, reference_data_element) ){
                         ptask_list.push_back( create_pseudotask(this_task, S_es, cond_it->second, dep->dst, var_pseudoname, pseudotask_count++, "in") );
                     }else{
                         /*
                          * JDF & QUARK specific optimization:
                          * Add the keyword "data_" infront of the matrix to
                          * differentiate the matrix from the struct.
                          */
#ifdef QUARK_COMPILER			 
                         printf("data_%s", tree_to_str(dep->dst));
#else
			 printf("%s", tree_to_str(dep->dst));

#endif
                     }
                 }
                 printf("\n");
#ifdef DEBUG_2
                 if( NULL != src_task ){
                     printf("          // %s -> %s ",src_task->task_name, tree_to_str(dep->dst));
                 }else{
                     printf("          // ENTRY -> %s ", tree_to_str(dep->dst));
                 }
                 (*dep->rel).print_with_subs(stdout);
#endif
             }

        }

        if(insert_fake_read){
            Relation emptyR;
            dep_t *dep = *(odeps.begin());
            printf(" <- ");
             /*
              * JDF & QUARK specific optimization:
              * Add the keyword "data_" infront of the matrix to
              * differentiate the matrix from the struct.
              */
            if( need_pseudotask(dep->src, reference_data_element) ){
                //printf("[[ data_%s ]]", tree_to_str(dep->src));
                ptask_list.push_back( create_pseudotask(this_task, S_es, emptyR, dep->src, var_pseudoname, pseudotask_count++, "in") );
            }else{
#ifdef QUARK_COMPILER		
                printf("data_%s\n",tree_to_str(dep->src));

#else
		printf("%s\n",tree_to_str(dep->src));

#endif
            }
        }

        // print the outgoing edges
        for (dep_it=odeps.begin(); dep_it!=odeps.end(); dep_it++){
             dep_t *dep = *dep_it;
             list< pair<expr_t *,Relation> > cond_list;
             list< pair<expr_t *,Relation> >::iterator cond_it, cond_it2;
             string cond;

             // Needed by Omega
             (void)(*dep->rel).print_with_subs_to_string(false);

             Q2J_ASSERT( NULL != dep->src->task );

             // If the condition has disjunctions (logical OR operators) then split them so that each one
             // is treated independently.
             cond_list = simplify_conditions_and_split_disjunctions(*dep->rel, S_es);
             for(cond_it = cond_list.begin(); cond_it != cond_list.end(); cond_it++){
                 string cond = expr_tree_to_str(cond_it->first);
                 bool printed_condition = false;

                 printf("         ");
                 printf(" -> ");

                 // TODO: If the conditions are mutually exclusive, we do not need to do the following step.

                 // For each condition that resulted from spliting a disjunction, negate
                 // all the other parts of the disjunction
                 for(cond_it2 = cond_list.begin(); cond_it2 != cond_it; cond_it2++){
                     string cond2 = expr_tree_to_str(cond_it2->first);
                     if( !cond2.empty() ){
                         if( printed_condition )
                             printf(" & ");
                         printf("(!(%s))",cond2.c_str());
                         printed_condition = true;
                     }
                 }
                 if( !cond.empty() ){
                     if( printed_condition )
                         printf(" & ");
                     printf("%s ? ",cond.c_str());
                 }else if( printed_condition ){
                     printf(" ? ");
                 }

                 node_t *sink = dep->dst;
                 if( NULL == sink ){
                     // EXIT

                     if( need_pseudotask(dep->src, reference_data_element) ){
                         //printf("[[ data_%s ]]", tree_to_str(dep->src));
                         ptask_list.push_back( create_pseudotask(this_task, S_es, cond_it->second, dep->src, var_pseudoname, pseudotask_count++, "out") );
                     }else{
                         /*
                          * JDF & QUARK specific optimization:
                          * Add the keyword "data_" infront of the matrix to
                          * differentiate the matrix from the struct.
                          */
#ifdef QUARK_COMPILER			 

                         printf("data_%s",tree_to_str(dep->src));
#else
                         printf("%s",tree_to_str(dep->src));
#endif
                     }
                 }else{
                     printf("%s %s(",sink->var_symname, sink->task->task_name);
                     print_actual_parameters(dep, relation_to_tree(cond_it->second), SINK);
                     printf(") ");
                 }
                 printf("\n");
#ifdef DEBUG_2
                 printf("       // ");
                 (*dep->rel).print_with_subs(stdout);
#endif
            }
        }

    }

    return ptask_list;
}

