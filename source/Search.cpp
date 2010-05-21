/*
 * Search.cpp
 *
 *  Created on: Sep 22, 2009
 *      Author: lars
 */

#undef DEBUG

#include "Search.h"
#include "ProgramOptions.h"

Search::Search(Problem* prob, Pseudotree* pt, SearchSpace* s, Heuristic* h) :
  m_nodesOR(0), m_nodesAND(0), m_nextThreadId(0), m_problem(prob),
  m_pseudotree(pt), m_space(s), m_heuristic(h) //, m_nextLeaf(NULL)
#ifdef PARALLEL_MODE
  , m_nextSubprob(NULL)
#endif
  {

#ifndef NO_CACHING
  // Init context cache table
//  cout << "Initialising cache system." << endl;
  m_space->cache = new CacheTable(prob->getN());
#endif

  // initialize the array for counting nodes per level
  m_nodeProfile.resize(m_pseudotree->getHeight()+1, 0);
  // initialize the array for counting leaves per level
  m_leafProfile.resize(m_pseudotree->getHeight()+1, 0);

  // initialize the local assignment vector for BaB
  m_assignment.resize(m_problem->getN(),NONE);
}


bool Search::doProcess(SearchNode* node) {
  assert(node);
  if (node->getType() == NODE_AND) {
    DIAG( ostringstream ss; ss << *node << " (l=" << node->getLabel() << ")\n"; myprint(ss.str()) );
    int var = node->getVar();
    int val = node->getVal();
    m_assignment[var] = val; // record assignment

    // dead end (AND label = 0)?
    if (node->getLabel() == ELEM_ZERO) {
      node->setLeaf(); // mark as leaf
      int depth = m_pseudotree->getNode(var)->getDepth();
      m_leafProfile.at(depth) += 1; // count leaf node
      PAR_ONLY( node->setSubLeaves(1) );
      return true; // and abort
    }
  } else { // NODE_OR
    DIAG( ostringstream ss; ss << *node << "\n"; myprint(ss.str()) );
  }

  return false; // default

}


bool Search::doCaching(SearchNode* node) {

  assert(node);
  int var = node->getVar();
  PseudotreeNode* ptnode = m_pseudotree->getNode(var);

  if (node->getType() == NODE_AND) { // AND node -> reset associated adaptive cache tables

    const list<int>& resetList = ptnode->getCacheReset();
    for (list<int>::const_iterator it=resetList.begin(); it!=resetList.end(); ++it)
      m_space->cache->reset(*it);

  } else { // OR node, try actual caching

    if (ptnode->getFullContext().size() <= ptnode->getParent()->getFullContext().size()) {
      // add cache context information
      addCacheContext(node,ptnode->getCacheContext());
      //DIAG( myprint( str("    Context set: ") + ptnode->getCacheContext() + "\n" ) );
      // try to get value from cache
      try {
        // will throw int(UNKNOWN) if not found
#ifndef NO_ASSIGNMENT
        pair<double,vector<val_t> > entry = m_space->cache->read(var, node->getCacheInst(), node->getCacheContext());
        node->setValue( entry.first ); // set value
        node->setOptAssig( entry.second ); // set assignment
#else
        double entry = m_space->cache->read(var, node->getCacheInst(), node->getCacheContext());
        node->setValue( entry ); // set value
#endif
        node->setLeaf(); // mark as leaf
#ifdef DEBUG
        {
          GETLOCK(mtx_io,lk);
          cout << "-Read " << *node
#ifndef NO_ASSIGNMENT
          << " with opt. solution " << node->getOptAssig()
#endif
          << endl;
        }
#endif
        return true;
      } catch (...) { // cache lookup failed
        node->setCachable(); // mark for caching later
      }
    }

  } // if on node type

  return false; // default, no caching applied

} // Search::doCaching


bool Search::doPruning(SearchNode* node) {

#ifdef NO_HEURISTIC
  return false;
#endif

  assert(node);
  int var = node->getVar();
  PseudotreeNode* ptnode = m_pseudotree->getNode(var);
  int depth = ptnode->getDepth();

  if (canBePruned(node)) {
    DIAG( myprint("\t !pruning \n") );
    node->setLeaf();
    node->setPruned();
    node->setValue(ELEM_ZERO);
    if (node->getType() == NODE_AND) {
      // count 1 leaf AND node
      m_leafProfile.at(depth) += 1;
      PAR_ONLY( node->setSubLeaves(1) ) ;
    } else { // NODE_OR
      // assume all AND children would have been created and pruned
      m_leafProfile.at(depth) += m_problem->getDomainSize(var);
      PAR_ONLY( node->addSubLeaves(m_problem->getDomainSize(var)) ) ;
    }
    return true;
  }

  return false; // default false

} // Search::doPruning



SearchNode* Search::nextLeaf() {

  SearchNode* node = NULL;
  while (true) {
    node = this->nextNode();
    if (doProcess(node)) // initial processing
      { return node; }
    if (doCaching(node)) // caching?
      { return node; }
    if (doPruning(node)) // pruning?
      { return node; }
    if (doExpand(node)) // node expansion
      { return node; }
  }

}


bool Search::canBePruned(SearchNode* n) const {

  // heuristic is upper bound, hence can use to prune if value=zero
  if (n->getHeur() == ELEM_ZERO)
    return true;

  SearchNode* curAND;
  SearchNode* curOR;
  double curPSTVal;

  if (n->getType() == NODE_AND) {
    curAND = n;
    curOR = n->getParent();
    curPSTVal = curAND->getHeur(); // includes label
  } else { // NODE_OR
    curAND = NULL;
    curOR = n;
    curPSTVal = curOR->getHeur(); // n->getHeur()
  }

  DIAG( ostringstream ss; ss << "\tcanBePruned(" << *n << ")" << " h=" << n->getHeur() << endl; myprint(ss.str()); )

  list<SearchNode*> notOptOR; // marks nodes for tagging as possibly not optimal

  // up to root node, if we have to
  while (true) {
    DIAG( ostringstream ss; ss << "\t ?PST root: " << *curOR << " pst=" << curPSTVal << " v=" << curOR->getValue() << endl; myprint(ss.str()); )

    //if ( fpLt(curPSTVal, curOR->getValue()) ) {
    if ( curPSTVal <= curOR->getValue() ) {
      for (list<SearchNode*>::iterator it=notOptOR.begin(); it!=notOptOR.end(); ++it)
        (*it)->setNotOpt(); // mark as possibly not optimal
      return true; // pruning is possible!
    }

    // check if moving up in search space is possible
    if (! curOR->getParent())
      { return false; }

    notOptOR.push_back(curOR);

    // climb up, update values
    curAND = curOR->getParent();

    // collect AND node label
    curPSTVal OP_TIMESEQ curAND->getLabel();
    // incorporate already solved sibling OR nodes
    curPSTVal OP_TIMESEQ curAND->getSubSolved();

    // incorporate new not-yet-solved sibling OR nodes through their heuristic
    const CHILDLIST& children = curAND->getChildren();
    for (CHILDLIST::const_iterator it=children.begin(); it!=children.end(); ++it) {
      if (*it == curOR) continue; // skip previous OR node on current path
      else curPSTVal OP_TIMESEQ (*it)->getHeur();
    }

    curOR = curAND->getParent();
  }

  // default (should never get to this line since false is returned inside loop)
  assert(false);
  return false;

} // Search::canBePruned



double Search::heuristicOR(SearchNode* n) {

  int v = n->getVar();
  double d;
  double* dv = new double[m_problem->getDomainSize(v)*2];
  double h = ELEM_ZERO; // the new OR nodes h value
  for (val_t i=0;i<m_problem->getDomainSize(v);++i) {
    m_assignment[v] = i;

    // compute heuristic value
    dv[2*i] = m_heuristic->getHeur(v,m_assignment);

    // precompute label value
    d = ELEM_ONE;
    const list<Function*>& funs = m_pseudotree->getFunctions(v);
    for (list<Function*>::const_iterator it = funs.begin(); it != funs.end(); ++it)
    {
      d OP_TIMESEQ (*it)->getValue(m_assignment);
    }

    // store label and heuristic into cache table
    dv[2*i+1] = d; // label
    dv[2*i] OP_TIMESEQ d; // heuristic

    if (dv[2*i] > h)
        h = dv[2*i]; // keep max. for OR node heuristic
  }

  n->setHeur(h);
  n->setHeurCache(dv);

  return h;

} // Search::heuristicOR


#ifndef NO_CACHING
void Search::addCacheContext(SearchNode* node, const set<int>& ctxt) const {

  context_t sig;
  sig.reserve(ctxt.size());
  for (set<int>::const_iterator itC=ctxt.begin(); itC!=ctxt.end(); ++itC) {
    sig.push_back(m_assignment[*itC]);
  }

  node->setCacheContext(sig);
  node->setCacheInst( m_space->cache->getInstCounter(node->getVar()) );

}
#endif /* NO_CACHING */


#ifdef PARALLEL_MODE
void Search::addSubprobContext(SearchNode* node, const set<int>& ctxt) const {

  context_t sig;
  sig.reserve(ctxt.size());
  for (set<int>::const_iterator itC=ctxt.begin(); itC!=ctxt.end(); ++itC) {
    sig.push_back(m_assignment[*itC]);
  }
  node->setSubprobContext(sig);
}
#endif /* PARALLEL_MODE */


double Search::lowerBound(SearchNode* node) const {

  // assume OR node
  assert(node->getType() == NODE_OR);

  double maxBound = ELEM_ZERO;
  maxBound = max(maxBound, node->getValue()); // to deal with NaN
  double curBound = ELEM_ZERO;
  double pstVal = ELEM_ONE;

  vector<double> pst;
  node->getPST(pst); // will store PST into pst

  // climb up to search space root node
  for (vector<double>::iterator it=pst.begin(); it!=pst.end(); ) {

    // iterator is at AND position
    pstVal OP_TIMESEQ (*it); // add current label

    ++it; // move to OR value
    curBound = (*it) OP_DIVIDE pstVal; // current bound
    maxBound = max(maxBound,curBound); // order is important (for NaN)

    ++it; // move to next AND label
  }

  return maxBound;

}


bool Search::loadInitialBound(string file) {

  // See if file can be opened
  ifstream inTest(file.c_str());
  inTest.close();
  if (inTest.fail()) {
    cerr << "ERROR reading from file " << file << endl;
    return false;
  }

  // Read the actual value from file
  igzstream infile(file.c_str(), ios::in | ios::binary);
  if (infile) {
    double bound;
    BINREAD(infile, bound);
    this->setInitialBound(bound);

#ifndef NO_ASSIGNMENT
    // read optimal assignment from file
    int noVars;
    BINREAD(infile, noVars);

    if (noVars != m_problem->getNOrg()) {
      cerr << "ERROR reading SLS solution, number of vars doesn't match" << endl;
      return false;
    }

    // read full tuple
    vector<int> tuple; tuple.reserve(noVars);
    for (int i=0; i<noVars; ++i) {
      int x;
      BINREAD(infile,x);
      tuple.push_back(x);
    }

    // filter evidence and unary domain variables that were removed during preprocessing
    vector<val_t> reduced;
    for (int i=0; i<noVars; ++i) {
      if (!m_problem->isEliminated(i)) {
        reduced.push_back( (val_t) tuple.at(i));
      }
    }

    // dummy node = 0
    reduced.push_back((val_t) 0);

    if ((int) reduced.size() != m_problem->getN()) {
      cerr << "ERROR reading SLS solution, reduced problem size doesn't match" << endl;
      return false;
    }

    // store into search space
    this->setInitialSolution(reduced);

#endif
  }
  infile.close();
  return true;

}


/* restricts the search space to a subproblem as specified:
 * @rootVar: the subproblem root variable
 * @assig: variable assignment, possibly partial, needed for context instantiation
 * @pst: the parent partial solution tree, interpreted *top-down* !
 */
int Search::restrictSubproblem(int rootVar, const vector<val_t>& assig, const vector<double>& pst) {

  // adjust the pseudo tree, returns original depth of new root node
  int depth = m_pseudotree->restrictSubproblem(rootVar);

  // resize node count vectors for subproblem
  m_nodeProfile.resize(m_pseudotree->getHeightCond()+1);
  m_leafProfile.resize(m_pseudotree->getHeightCond()+1);

  // set context assignment
  const set<int>& context = m_pseudotree->getNode(rootVar)->getFullContext();
  for (set<int>::const_iterator it = context.begin(); it!=context.end(); ++it) {
    m_assignment[*it] = assig[*it];
  }

  // generate structure of bogus nodes to hold partial solution tree copied
  // from master search space

  SearchNode* next = this->nextNode(); // dummy AND node on top of stack
  SearchNode* node = NULL;

  int pstSize = pst.size() / 2;

  // build structure top-down, first entry in pst array is assumed to correspond to
  // highest OR value, last entry is the lowest AND label
  for (int i=0; i<pstSize; ++i) {

    // add dummy OR node with proper value
    node = next;
    next = new SearchNodeOR(node, node->getVar()) ;
    next->setValue(pst.at(2*i));
//    cout << " Added dummy OR node with value " << d1 << endl;
    node->addChild(next);

    node = next;
    next = new SearchNodeAND(node, 0, pst.at(2*i+1)) ;
//    cout << " Added dummy AND node with label " << d2 << endl;
    node->addChild(next);

  }

  // create another set of dummy nodes as a buffer for the subproblem
  // value since the previous dummy nodes might have non-empty
  // labels/values from the parent problem)
  node = next;
  next = new SearchNodeOR(node, node->getVar());
  node->addChild(next);
  node = next;
  next = new SearchNodeAND(node, 0); // label = ELEM_ONE !
  node->addChild(next);

  m_space->subproblemLocal = node; // dummy OR node

  // empty existing queue/stack/etc. and add new node
  this->resetSearch(next); // dummy AND node

  return depth;

}



bool Search::restrictSubproblem(const string& file) {
  assert(!file.empty());

  {
    ifstream inTemp(file.c_str());
    inTemp.close();

    if (inTemp.fail()) {
      cerr << "Error opening subproblem specification " << file << '.' << endl;
      return false;
    }
  }

//  string line;
  igzstream fs(file.c_str(), ios::in | ios::binary);

  int rootVar = UNKNOWN;
//  int noVars = UNKNOWN;

  //////////////////////////////////////
  // FIRST: read context of subproblem

  // root variable of subproblem
  BINREAD(fs, rootVar);
  if (rootVar<0 || rootVar>= m_problem->getN()) {
    cerr << "ERROR reading subproblem specification, variable index " << rootVar << " out of range" << endl;
    return false;
  }

  cout << "Restricting to subproblem with root node " << rootVar << endl;

  int x = UNKNOWN;
  // context size
  BINREAD(fs, x);
  const set<int>& context = m_pseudotree->getNode(rootVar)->getFullContext();
  if (x != (int) context.size()) {
    cerr << "ERROR reading subproblem specification, context size doesn't match." << endl;
    return false;
  }

  // Read context assignment into temporary vector
  vector<val_t> assignment(m_problem->getN(), UNKNOWN);
  val_t y = UNKNOWN;
  int z = UNKNOWN;
  cout << "Subproblem context:";
  for (set<int>::const_iterator it = context.begin(); it!=context.end(); ++it) {
    BINREAD(fs, z); // files always contain ints, convert to val_t
    y = (val_t) z;
    if (y<0 || y>=m_problem->getDomainSize(*it)) {
      cerr <<endl<< "ERROR reading subproblem specification, variable value " << (int)y << " not in domain." << endl;
      return false;
    }
    cout << ' ' << (*it) << "->" << (int) y;
    assignment[*it] = y;
  }
  cout << endl;

  ///////////////////////////////////////////////////////////////////////////
  // SECOND: read information about partial solution tree in parent problem

  // read encoded PST values for advanced pruning, and save into temporary vector.
  // the assumed order later is top-down; however, if pstSize from file is negative,
  // it's bottom-up in the file, so in that case we need to reverse
  double valueOR, labelAND;
  bool reverse = false;

  int pstSize = UNKNOWN;
  BINREAD(fs,pstSize);
  if (pstSize < 0) {
    reverse = true; // negative value indicates order needs to be reversed
    pstSize *= -1;
  }
  cout << "Reading parent partial solution tree of size " << pstSize << endl;

  vector<double> pstVals(pstSize*2, ELEM_NAN);

  if (reverse) { // fill array in reverse
    int i=pstSize-1;
    while (i >= 0) {
      BINREAD(fs, labelAND);
      BINREAD(fs, valueOR);
      pstVals.at(2*i+1) = labelAND;
      pstVals.at(2*i) = valueOR;
      i -= 1;
    }
  } else { // no reverse necessary
    int i=0;
    while (i<pstSize) {
      BINREAD(fs, valueOR);
      BINREAD(fs, labelAND);
      pstVals.at(2*i) = valueOR;
      pstVals.at(2*i+1) = labelAND;
      i += 1;
    }
  }

  fs.close();

  // call function to actually condition this search instance
  int depth = this->restrictSubproblem(rootVar, assignment, pstVals);

  cout << "Restricted to subproblem with root node " << rootVar << " at depth " << depth  << endl;

  return true; // success

}

