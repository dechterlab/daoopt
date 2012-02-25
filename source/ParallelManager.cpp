/*
 * ParallelManager.cpp
 *
 *  Copyright (C) 2008-2012 Lars Otten
 *  This file is part of DAOOPT.
 *
 *  DAOOPT is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  DAOOPT is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with DAOOPT.  If not, see <http://www.gnu.org/licenses/>.
 *  
 *  Created on: Apr 11, 2010
 *      Author: Lars Otten <lotten@ics.uci.edu>
 */

#undef DEBUG

#include "ParallelManager.h"

#ifdef PARALLEL_STATIC

/* parameters that can be modified */
#define MAX_SUBMISSION_TRIES 6
#define SUBMISSION_FAIL_DELAY_BASE 2.0

/* static definitions (no need to modify) */
#define CONDOR_SUBMIT "condor_submit"
#define CONDOR_WAIT "condor_wait"
#define CONDOR_RM "condor_rm"
/* some string definition for filenames */
#define PREFIX_SOL "temp_sol."
#define PREFIX_EST "temp_est."
#define PREFIX_SUB "temp_sub."
#define PREFIX_LOG "temp_log."
#define PREFIX_STATS "temp_stats."
#define PREFIX_SAMPLE "temp_sample."
#define PREFIX_LOWERBOUND "temp_lb."

#define PREFIX_JOBS "temp_jobs."

#define TEMPLATE_FILE "daoopt-template.condor"

/* custom attributes for generated condor jobs */
#define CONDOR_ATTR_PROBLEM "daoopt_problem"
#define CONDOR_ATTR_THREADID "daoopt_threadid"

/* default job submission template */
const string default_job_template = "\
    universe = vanilla \n\
    notification = never \n\
    should_transfer_files = yes \n\
    when_to_transfer_output = always \n\
    executable = daoopt.$$(Arch) \n\
    output = temp_out.%PROBLEM%.%TAG%.$(Process).txt \n\
    error = temp_err.%PROBLEM%.%TAG%.$(Process).txt \n\
    log = temp_log.%PROBLEM%.%TAG%.txt \n\
    +daoopt_problem = daoopt_%PROBLEM%_%TAG% \n\
    requirements = ( Arch == \"X86_64\" || Arch == \"INTEL\" ) \n\
    \n";

typedef pair<double, SearchNode*> PQEntry;

struct PQEntryComp {
  bool operator() (const PQEntry& lhs, const PQEntry& rhs) const {
    if (lhs.first < rhs.first)
      return true;
//    if (lhs.second->getHeur() < rhs.second->getHeur())
//      return true;
    return false;
  }
};


bool ParallelManager::storeLowerBound() const {
  oss fname;
  fname << PREFIX_LOWERBOUND << m_options->problemName << "." << m_options->runTag
      << ".sol.gz";
  m_problem->outputAndSaveSolution(fname.str(), NULL, m_nodeProfile,
                                   m_leafProfile, false);  // false -> no output to screen
  cout << "Wrote lower bound to file " << fname.str() << endl;
  return true;
}


bool ParallelManager::loadLowerBound() {
  oss fname;
  fname << PREFIX_LOWERBOUND << m_options->problemName << "." << m_options->runTag
      << ".sol.gz";
  cout << "Reading initial lower bound from file " << fname.str() << endl;
  this->loadInitialBound(fname.str());
  return true;
}


bool ParallelManager::doLearning() {
  if (m_options->sampleRepeat == 0 || m_options->sampleSizes.empty())
    return true;

#ifndef NO_ASSIGNMENT
  m_sampleSearch->updateSolution(this->getCurOptValue(), this->getCurOptTuple());
#else
  m_sampleSearch->updateSolution(this->getCurOptValue());
#endif

  BoundPropagator prop(m_problem, m_sampleSpace.get());

  vector<double> sampleSizes;
  for (int i = 0; i < m_options->sampleRepeat; ++i) {
    istringstream iss(m_options->sampleSizes);
    while (iss) {
      double d;
      iss >> d;
      iss.ignore(1);
      sampleSizes.push_back(d);
    }
  }

  // collect samples
  int sampleCount = 0;
  for (; sampleCount < sampleSizes.size(); ++sampleCount) {
    m_sampleSearch->reset();
    prop.resetSubCount();

    SearchNode* n = NULL;
    do  {
      n = m_sampleSearch->nextLeaf();
      if (!n) break;
      prop.propagate(n, false);
    } while (prop.getSubCountCache() < sampleSizes[sampleCount] * 100000);

    if (n) {
      cout << prop.getSubproblemStatsCache() << endl;
      m_learner->addSample(prop.getSubproblemStatsCache());
    } else {  // problem solved
      assert(false);  // TODO !
      break;
    }
  }

  // Output sample stats to file
  oss fname;
  fname << PREFIX_SAMPLE << m_options->problemName << '.' << m_options->runTag << ".csv";
  m_learner->statsToFile(fname.str());

  return true;
}


bool ParallelManager::restoreFrontier() {

  assert (!m_external.empty());

  // for output
  ostringstream ss;

  // records subproblems by (rootVar,context) and their id
  typedef hash_map<pair<int,context_t>, size_t > frontierCache;
  frontierCache subprobs;

  // filename for subproblem (=frontier)
  string subprobFile = filename(PREFIX_SUB,".gz");

  {
    ifstream inTemp(subprobFile.c_str());
    inTemp.close();
    if (inTemp.fail()) {
      ss.clear();
      ss << "Problem reading subprocess list from "<< subprobFile << '.' << endl;
      myerror(ss.str());
      return false;
    }
  }
  igzstream in(subprobFile.c_str(), ios::binary | ios::in);

  int rootVar = UNKNOWN;
  int x = UNKNOWN;
  int y = UNKNOWN;
  count_t count = NONE;
  count_t z = NONE;

  BINREAD(in, count); // total no. of subproblems

  for (size_t id=0; id<count; ++id) {

    BINREAD(in, z); // id
    BINREAD(in, rootVar); // root var
    BINREAD(in, x); // context size
    context_t context;
    for (int i=0;i<x;++i) {
      BINREAD(in,y); // read context value as int
      context.push_back((val_t) y); // cast to val_t
    }
    BINREAD(in, x); // PST size
    x = (x<0)? -2*x : 2*x;
    BINSKIP(in,double,x); // skip PST

    // store in local subproblem table
    subprobs.insert(make_pair( make_pair(rootVar,context) , id));

  }

  m_subprobCount = subprobs.size();
  ss.clear();
  ss << "Recovered " << m_subprobCount << " subproblems from file " << subprobFile << endl;
  myprint(ss.str());


  //////////////////////////////////////////////////////////////////////
  // part 2: now expand search space until stored frontier nodes found
  SearchNode* node = m_external.back();
  m_external.pop_back();

  PseudotreeNode* ptnode = NULL;

  ptnode = m_pseudotree->getNode(node->getVar());

#ifndef NO_HEURISTIC
  // precompute heuristic of initial dummy OR node (couldn't be done earlier)
  assignCostsOR(node);
#endif

  stack<SearchNode*> dfs;
  dfs.push(node);

  // intermediate vector for expanding nodes into
  vector<SearchNode*> newNodes;
  count = 0;

  // prepare m_external vector for frontier nodes
  m_external.clear();
  m_external.resize(subprobs.size(),NULL);


  while (!dfs.empty() ) { // && count != subprobs.size() ) { // TODO?

    node = dfs.top();
    dfs.pop();

    syncAssignment(node);
    x = node->getVar();
    ptnode = m_pseudotree->getNode(x);
    addSubprobContext(node,ptnode->getFullContextVec());

    // check against subproblems from saved list
    frontierCache::iterator lkup = subprobs.find(make_pair(x,node->getSubprobContext()));
    if (lkup != subprobs.end()) {
      m_external[lkup->second] = node;
//      cout << "External " << lkup->second << " = " << node << endl;
      count += 1;
      continue;
    }

    deepenFrontier(node,newNodes);
    for (vector<SearchNode*>::iterator it=newNodes.begin(); it!=newNodes.end(); ++it)
      dfs.push(*it);
    newNodes.clear();

  } // end while

  if (count != subprobs.size()) {
    ss.clear();
    ss << "Warning: only " << count << " frontier nodes." << endl;
    myprint(ss.str());
  }
  if (!dfs.empty()) {
    ss.clear();
    ss << "Warning: Stack still has " << dfs.size() << " nodes." << endl;
    myprint(ss.str());
  }

  return true;
}


bool ParallelManager::findFrontier() {

  assert(!m_external.empty());

  SearchNode* node = m_external.back();
  m_external.pop_back();
  double eval = 42.0;

  // queue of subproblems, i.e. OR nodes, ordered according to
  // evaluation function
  priority_queue<PQEntry, vector<PQEntry>, PQEntryComp> m_open;
  m_open.push(make_pair(eval,node));

#ifndef NO_HEURISTIC
  // precompute heuristic of initial dummy OR node (couldn't be done earlier)
  assignCostsOR(node);
#endif

  // intermediate container for expanding nodes into
  vector<SearchNode*> newNodes;

  // split subproblems
  while (m_open.size()
      && (m_options->threads == NONE
          || (int) m_open.size() < m_options->threads)) {

    eval = m_open.top().first;
    node = m_open.top().second;
    m_open.pop();
    DIAG(oss ss; ss << "Top " << node <<"(" << *node << ") with eval " << eval << endl; myprint(ss.str());)

    // check for fixed-depth cutoff
    if (m_options->cutoff_depth != NONE) {
      PseudotreeNode* ptnode = m_pseudotree->getNode(node->getVar());
      int d = ptnode->getDepth();
      if (d == m_options->cutoff_depth) {
        node->setExtern();
        m_external.push_back(node);
        continue;
      }
    }

    syncAssignment(node);
    deepenFrontier(node,newNodes);
    for (vector<SearchNode*>::iterator it=newNodes.begin(); it!=newNodes.end(); ++it) {
      (*it)->setInitialBound(lowerBound(*it));  // store bound at time of evaluation
      m_open.push(make_pair(evaluate(*it),*it) );
    }
    newNodes.clear();

  }

  // move from open stack to external queue
  m_external.reserve(m_open.size());
  while (m_open.size()) {
    m_open.top().second->setExtern();
    m_external.push_back(m_open.top().second);
    m_open.pop();
  }

  m_subprobCount = m_external.size();

  ostringstream ss;
  ss << "Generated " << m_subprobCount << " external subproblems, " << m_local.size() << " local" << endl;
  myprint(ss.str());

  for (vector<SearchNode*>::iterator it = m_local.begin(); it!=m_local.end(); ++it) {
    solveLocal(*it);
  }
  myprint("Local jobs (if any) done.\n");

  return true;
}


bool ParallelManager::runCondor() const {

  // generates files for grid jobs and submits
  if (m_external.size()) {
    myprint("Submitting to Condor...\n");
    if (!submitToGrid())
      return false;
  }

  if (m_subprobCount) {
    myprint("Waiting for Condor...\n");
    // wait for grid jobs to finish
    if (!waitForGrid())
      return false;
//    myprint("External jobs done.\n");
    myprint("Condor jobs done.\n");
  }

  return true; // default: success
}


bool ParallelManager::writeJobs() const {

//  const vector<SearchNode*>& nodes = m_external;

  // Build the condor job description
  ostringstream jobstr;

  // Try to read template file, otherwise use default
  ifstream templ(TEMPLATE_FILE);
  if (templ) {
    string s;
    while (!templ.eof()) {
      getline(templ, s);
      s = str_replace(s,"%PROBLEM%",m_options->problemName);
      s = str_replace(s,"%TAG%",m_options->runTag);
      jobstr << s << endl;
    }
  } else {
    string s = default_job_template;
    s = str_replace(s,"%PROBLEM%",m_options->problemName);
    s = str_replace(s,"%TAG%",m_options->runTag);
    jobstr << s;
  }
  templ.close();

  // encode actual subproblems
  string alljobs = encodeJobs(m_external);
  jobstr << alljobs;

  // write subproblem file
  string jobfile = filename(PREFIX_JOBS,".condor");
  ofstream file(jobfile.c_str());
  if (!file) {
    myerror("Error writing Condor job file.\n");
    return false;
  }
  file << jobstr.str();
  file.close();

  // CSV file with subproblem stats
  writeStatsCSV(m_external);

  return true;
}


bool ParallelManager::submitToGrid() const {

  string jobfile = filename(PREFIX_JOBS,".condor");

  // Perform actual submission
  ostringstream submitCmd;
  submitCmd << CONDOR_SUBMIT << ' ' << jobfile;
  submitCmd << " > /dev/null";

  int noTries = 0;
  bool success = false;

  while (!success) {

    if ( !system( submitCmd.str().c_str() ) ) // returns 0 if successful
      success = true;

    if (!success) {
      if (noTries++ == MAX_SUBMISSION_TRIES) {
        ostringstream ss;
        ss << "Failed invoking condor_submit." << endl;
        myerror(ss.str());
        return false;
      } else {
        myprint("Problem invoking condor_submit, retrying...\n");
        sleep(pow( SUBMISSION_FAIL_DELAY_BASE ,noTries+1)); // wait increases exponentially
      }
    } else {
      // submission successful
      ostringstream ss;
      ss << "----> Submitted " << m_subprobCount << " jobs." << endl;
      myprint(ss.str());
    }

  } // while (!success)

  return true; // default: success
}


bool ParallelManager::waitForGrid() const {

  string logfile = filename(PREFIX_LOG,".txt");

  ostringstream waitCmd;
  waitCmd << CONDOR_WAIT << ' ' << logfile;
  waitCmd << " > /dev/null";

  // wait for the job to finish
  if ( system(waitCmd.str().c_str()) ) {
    cerr << "Error invoking "<< CONDOR_WAIT << endl;
    return false; // error!
  }

  return true; // default: success

}


bool ParallelManager::readExtResults() {
  myprint("Parsing external results.\n");

  vector<pair<count_t, count_t> > nodecounts;
  nodecounts.reserve(m_external.size());

  bool success = true;
  for (size_t id=0; id<m_subprobCount; ++id) {

    SearchNode* node = m_external.at(id);

    // Read solution from file
    string solutionFile = filename(PREFIX_SOL,".gz",id);
    {
      ifstream inTemp(solutionFile.c_str());
      inTemp.close();
      if (inTemp.fail()) {
        ostringstream ss;
        ss << "Solution file " << id << " unavailable: " << solutionFile << endl;
        myerror(ss.str());
        node->setErrExt();  // to suppress CSV output
        success = false;
        continue; // skip rest of loop
      }
    }
    igzstream in(solutionFile.c_str(), ios::binary | ios::in);

    double optCost;
    BINREAD(in, optCost); // read opt. cost
//    if (ISNAN(optCost)) optCost = ELEM_ZERO;

    count_t nodesOR, nodesAND;
    BINREAD(in, nodesOR);
    BINREAD(in, nodesAND);

    nodecounts.push_back(make_pair(nodesOR, nodesAND));

    m_space->stats.numORext += nodesOR;
    m_space->stats.numANDext += nodesAND;

#ifndef NO_ASSIGNMENT
    int32_t n;
    BINREAD(in, n); // read length of opt. tuple

    vector<val_t> tup(n,UNKNOWN);

    int32_t v; // assignment saved as int, regardless of internal type
    for (int i=0; i<n; ++i) {
      BINREAD(in, v); // read opt. assignments
      tup[i] = (val_t) v;
    }

    // Check external tuple size, but allow zero (from NaN solutions)
    size_t subsize = m_pseudotree->getNode(node->getVar())->getSubprobVars().size();
    if (tup.size() > 0 && tup.size() != subsize) {
      oss ss;
      ss << "Solution file " << id << " length mismatch, got " << tup.size()
         << ", expected " << subsize << endl;
      myprint(ss.str());
      node->setErrExt();  // to suppress CSV output
      success = false;
      continue;
    }
#endif

    // read node profiles
    int32_t size;
    count_t c;
    BINREAD(in, size);
    vector<count_t> leafP, nodeP;
    leafP.reserve(size);
    nodeP.reserve(size);
    for (int i=0; i<size; ++i) { // leaf profile
      BINREAD(in, c);
      leafP.push_back(c);
    }
    for (int i=0; i<size; ++i) { // full node profile
      BINREAD(in, c);
      nodeP.push_back(c);
    }

    // done reading input file
    in.close();

    // Write subproblem solution value and tuple into search node
    node->setValue(optCost);
#ifndef NO_ASSIGNMENT
    node->setOptAssig(tup);
#endif

    ostringstream ss;
    ss  << "Solution file " << id << " read (" << *node
        << ") " << nodesOR << " / " << nodesAND
        << " v:" << node->getValue();
#ifndef NO_ASSIGNMENT
    DIAG(ss << " -assignment " << node->getOptAssig());
#endif
    ss << endl;
    myprint(ss.str());

    // propagate result (but don't delete node, needed for stats)
    m_prop.propagate(node, true, node);
  }

  myprint("Writing CSV stats.\n");
  writeStatsCSV(m_external, &nodecounts);

  return success;
}


void ParallelManager::writeStatsCSV(const vector<SearchNode*>& subprobs,
                                    const vector<pair<count_t, count_t> >* counts) const {
  if (counts)
    assert(subprobs.size() == counts->size());

  // open CSV file for stats
  string csvfile = filename(PREFIX_STATS,".csv");
  ofstream csv(csvfile.c_str(), ios_base::out | ios_base::trunc);

  csv // << "idx" << '\t'
    << "root" << '\t' << "ibnd"
    << '\t' << "lb" << '\t' << "ub"
    << '\t' << "rPruned" << '\t' << "rDead" << '\t' << "rLeaf"
    << '\t' << "avgNodeD" << '\t' << "avgLeafD" << '\t' << "avgBraDg"
    << '\t' << "D";
  BOOST_FOREACH( const string& s, SubprobStats::legend ) {
    csv << '\t' << s;
  }
  csv << '\t' << "est";
  if (counts) {
    csv << '\t' << "or" << '\t' << "and";
  }
  csv << endl;

  for (size_t i = 0; i < subprobs.size(); ++i) {
    SearchNode* node = subprobs.at(i);
    assert(node);

    if (node->isErrExt())  // marked in readExtResults()
      continue;  // skip problematic nodes in CSV output

    int rootVar = node->getVar();
    PseudotreeNode* ptnode = m_pseudotree->getNode(rootVar);
    const SubprobStats* stats = ptnode->getSubprobStats();

    SubprobFeatures* dynamicFeatures = node->getSubprobFeatures();

    int depth = ptnode->getDepth();
    double lb = node->getInitialBound(); //lowerBound(node);
    double ub = node->getHeur();
    double estimate = evaluate(node);

    csv // << i << '\t'
      << rootVar << '\t' << m_options->ibound
      << '\t' << lb << '\t' << ub
      << '\t' << dynamicFeatures->ratioPruned
      << '\t' << dynamicFeatures->ratioDead
      << '\t' << dynamicFeatures->ratioLeaf
      << '\t' << dynamicFeatures->avgNodeDepth
      << '\t' << dynamicFeatures->avgLeafDepth
      << '\t' << dynamicFeatures->avgBranchDeg
      << '\t' << depth;
    BOOST_FOREACH ( double d, stats->getAll() ) {
      csv << '\t' << d;
    }
    csv << '\t' << estimate;

    if (counts)  // solution node counts, if available
      csv << '\t' << counts->at(i).first  // OR nodes
          << '\t' << counts->at(i).second;  // AND nodes

    csv << endl;
  }
  csv.close();
}


string ParallelManager::encodeJobs(const vector<SearchNode*>& nodes) const {
  // Will hold the condor job description for the submission file
  ostringstream job;
  string subprobsFile = filename(PREFIX_SUB,".gz");
  ogzstream subprobs(subprobsFile.c_str(), ios::out | ios::binary);
  if (!subprobs) {
    ostringstream ss;
    ss << "Problem writing subproblem file"<< endl;
    myerror(ss.str());
    return "";
  }

  // no. of subproblems to file (id variable used as temp)
  count_t id=nodes.size();
  BINWRITE(subprobs,id);

  id = 0;
  int z = NONE;
  vector<val_t> assign(m_assignment.size(),NONE);

  // write subproblem file and csv statistics
  for (vector<SearchNode*>::const_iterator it=nodes.begin(); it!=nodes.end(); ++it, ++id) {

    const SearchNode* node = (*it);
    assert(node && node->getType()==NODE_OR);

    { // extract node context assignment
      const SearchNode* n2 = node;
      while (n2->getParent()) {
        n2 = n2->getParent(); // AND node
        assign.at(n2->getVar()) = n2->getVal();
        n2 = n2->getParent(); // OR node
      }
    }

    // write current subproblem id
    BINWRITE(subprobs, id);

    int rootVar = node->getVar();
    PseudotreeNode* ptnode = m_pseudotree->getNode(rootVar);

    /* subproblem root variable */
    BINWRITE( subprobs, rootVar );
    /* subproblem context size */
    const vector<int>& ctxt = ptnode->getFullContextVec();
    int sz = ctxt.size();
    BINWRITE( subprobs, sz );

    /* write context instantiation */
    for (vector<int>::const_iterator it=ctxt.begin(); it!=ctxt.end(); ++it) {
      z = (int) assign.at(*it);
      BINWRITE( subprobs, z);
    }

    /* write the PST:
     * first, number of entries = depth of root node
     * write negative number to signal bottom-up traversal
     * (positive number means top-down, to be compatible with old versions)
     */
    vector<double> pst;
    node->getPST(pst);
    int pstSize = ((int) pst.size()) / -2;
    BINWRITE( subprobs, pstSize); // write negative PST size

    for(vector<double>::iterator it=pst.begin();it!=pst.end();++it) {
      BINWRITE( subprobs, *it);
    }
  } // for-loop over nodes

  // close subproblem file
  subprobs.close();

  // job string for submission file
  job // special condor attribute
    << '+' << CONDOR_ATTR_THREADID << " = $(Process)" << endl
    // Make sure input files get transferred
    << "transfer_input_files = " << m_options->out_reducedFile
    << ", " << m_options->in_orderingFile
    << ", " << subprobsFile;
  if (!m_options->in_evidenceFile.empty())
    job << ", " << m_options->in_evidenceFile;
  job << endl;

  string solutionFile = filename(PREFIX_SOL,".$(Process).gz");

  job // command line arguments
    << "arguments = "
    << " -f " << m_options->out_reducedFile;
  if (!m_options->in_evidenceFile.empty())
    job << " -e " << m_options->in_evidenceFile;
  job
    << " -o " << m_options->in_orderingFile
    << " -s " << subprobsFile << ":$(Process)"
    << " -i " << m_options->ibound
    << " -j " << m_options->cbound_worker
    << " -c " << solutionFile
    << " -r " << m_options->subprobOrder
    << " -t 0" << endl;

  job << "queue " << m_subprobCount << endl;
  return job.str();
}


bool ParallelManager::isEasy(const SearchNode* node) const {
  assert(node);

  int var = node->getVar();
  PseudotreeNode* ptnode = m_pseudotree->getNode(var);
//  int depth = ptnode->getDepth();
//  int height = ptnode->getSubHeight();
  int width = ptnode->getSubWidth();

  if (width <= m_options->cutoff_width)
    return true;

  return false; // default

}


/* expands a node -- assumes OR node! Will generate descendant OR nodes
 * (via intermediate AND nodes) and put them in the referenced vector */
bool ParallelManager::deepenFrontier(SearchNode* n, vector<SearchNode*>& out) {

  if (!out.empty()) {
    cerr << "Warning: deepenFrontier() discarding nodes?" << endl;
    out.clear();
  }

  assert(n && n->getType() == NODE_OR);
  vector<SearchNode*> chi, chi2;

  // first generate intermediate AND nodes
  if (generateChildrenOR(n,chi)) {
    m_prop.propagate(n, true);
    return true; // no children
  }

  // for each AND node, generate OR children
  for (vector<SearchNode*>::iterator it=chi.begin(); it!=chi.end(); ++it) {
    DIAG(oss ss; ss << '\t' << *it << ": " << *(*it) << " (l=" << (*it)->getLabel() << ")" << endl; myprint(ss.str());)

    doProcess(*it);
    if (generateChildrenAND(*it,chi2)) {
      m_prop.propagate(*it, true);
      continue;
    }
    DIAG( for (vector<SearchNode*>::iterator it=chi2.begin(); it!=chi2.end(); ++it) {oss ss; ss << "\t  " << *it << ": " << *(*it) << endl; myprint(ss.str());} )
    for (vector<SearchNode*>::iterator it=chi2.begin(); it!=chi2.end(); ++it) {

      // Apply LDS if mini buckets are accurate
      if (applyLDS(*it)) {
        m_prop.propagate(*it, true);
        continue; // skip to next
      }
      // Apply AOBB to sample dynamic features
      if (applyAOBB(*it, m_options->aobbLookahead)) {
        m_prop.propagate(*it, true);
        continue;
      }

      if (doCaching(*it)) {
        m_prop.propagate(*it,true); continue;
      } else if (doPruning(*it)) {
        m_prop.propagate(*it,true); continue;
      } else if (isEasy(*it)) {
        m_local.push_back(*it);
      } else {
        out.push_back(*it);
      }
    }
    chi2.clear(); // prepare for next AND node
  }

  return false; // default false
}


double ParallelManager::evaluate(const SearchNode* node) const {
  assert(node && node->getType() == NODE_OR);

  if (m_options->cutoff_depth != NONE)
    return 0.0;

  int var = node->getVar();
  PseudotreeNode* ptnode = m_pseudotree->getNode(var);
  const SubprobStats* stats = ptnode->getSubprobStats();
  const SubprobFeatures* feats = node->getSubprobFeatures();

  // "dynamic subproblem features
  double ub = node->getHeur(),
         lb = node->getInitialBound();
  double rPruned = feats->ratioPruned,
         rLeaf = feats->ratioLeaf,
         rDead = feats->ratioLeaf;
  double avgNodeD = feats->avgNodeDepth,
         avgLeafD = feats->avgLeafDepth,
         avgBraDg = feats->avgBranchDeg;

  // "static" subproblem properties
  double D = node->getDepth(),
         Vars = ptnode->getSubprobSize(),
         Leafs = stats->getLeafCount();
  double Wmax = stats->getClusterStats(stats->MAX),
         Wavg = stats->getClusterStats(stats->AVG),
         Wsdv = stats->getClusterStats(stats->SDV),
         Wmed = stats->getClusterStats(stats->MED);
  double WCmax = stats->getClusterCondStats(stats->MAX),
         WCavg = stats->getClusterCondStats(stats->AVG),
         WCsdv = stats->getClusterCondStats(stats->SDV),
         WCmed = stats->getClusterCondStats(stats->MED);
  double Kmax = stats->getDepthStats(stats->MAX),
         Kavg = stats->getDepthStats(stats->AVG),
         Ksdv = stats->getDepthStats(stats->SDV);
  double Hmax = stats->getDepthStats(stats->MAX),
         Havg = stats->getDepthStats(stats->AVG),
         Hsdv = stats->getDepthStats(stats->SDV),
         Hmed = stats->getDepthStats(stats->MED);

  /*
  double z = (lb == ELEM_ZERO) ? 0.0 : 2*(ub-lb);
  z += Hmax + 0.5 * log10(Vars);
  */

  double z = // from LassoLars(alpha=0.001, normalize=True, fit_intercept=False), stats3.csv (4600 samples)
      0.00144 * lb*lb  - 0.00134  * lb*avgNodeD  - 0.00962  * ub*(ub-lb)  - 0.03766 * ub*rPruned  - 0.00566 * ub*avgLeafD  + 0.00256  * ub*Vars  - 0.06061  * ub*Wmax  + 0.01577  * ub*WCmax  - 0.00284 * ub*Havg  + 0.11317  * ub-lb*avgNodeD  - 0.09876 * (ub-lb)*avgLeafD  + 0.01903 * (ub-lb)*D  + 0.00217  * (ub-lb)*Vars  - 0.01307 * (ub-lb)*Leafs  - 0.01797  * (ub-lb)*Wmax  + 0.03154 * (ub-lb)*WCmax  - 0.00227  * (ub-lb)*Havg  - 0.00954 * rPruned*Vars  + 0.04717 * rPruned*Hmax  + 0.02503 * rDead*Vars  + 0.00012 * rLeaf*Vars  + 0.01730 * avgNodeD*avgNodeD  - 0.04324  * avgNodeD*avgLeafD  - 0.09762  * avgNodeD*D  + 0.00239 * avgNodeD*Vars  - 0.03318  * avgNodeD*Wmax  - 0.02509* avgNodeD*Hmax  + 0.03100  * avgNodeD*Hmed  + 0.02819  * avgLeafD*avgLeafD  + 0.10540  * avgLeafD*D  - 0.00515 * avgLeafD*Vars  + 0.00282  * avgLeafD*Leafs  + 0.07349 * avgLeafD*Wmax  - 0.03486  * avgLeafD*WCmax  + 0.01079 * avgLeafD*Havg  - 0.07081  * avgBraDg*Leafs  - 0.02872 * D*D  - 0.00984  * D*Leafs  + 0.00073  * Vars*Vars  - 0.00017  * Vars*Wmed  - 0.07097  * Vars*WCavg  + 0.03487 * Vars*WCsdv  + 0.00607 * Vars*Ksdv  + 0.00508  * Vars*Hmax  - 0.00791  * Vars*Hmed  - 0.00369  * Leafs*Leafs  + 0.00442  * Leafs*Hmax  - 0.01998 * Wmax*Hmed  - 0.01152  * WCmax*Hmed  + 0.00362 * Hmax*Hmax;

  DIAG(oss ss; ss<< "eval " << *node << " : "<< z<< endl; myprint(ss.str()))

  return z;

}


void ParallelManager::resetLocalStack(SearchNode* node) {
  while (!m_stack.empty())
    m_stack.pop();
  if (node)
    m_stack.push(node);
}


void ParallelManager::solveLocal(SearchNode* node) {
  assert(node && node->getType()==NODE_OR);
  DIAG(ostringstream ss; ss << "Solving subproblem locally: " << *node << endl; myprint(ss.str()));
  syncAssignment(node);
  this->resetLocalStack(node);
  while ( ( node=nextLeaf() ) )
    m_prop.propagate(node,true);
}


bool ParallelManager::doExpand(SearchNode* n) {

  assert(n);
  m_expand.clear();

  if (n->getType() == NODE_AND) { /////////////////////////////////////

    if (generateChildrenAND(n, m_expand))
      return true; // no children

    for (vector<SearchNode*>::iterator it = m_expand.begin(); it != m_expand.end(); ++it)
      m_stack.push(*it);

  } else { // NODE_OR /////////////////////////////////////////////////

    if (generateChildrenOR(n, m_expand))
      return true; // no children

    for (vector<SearchNode*>::iterator it = m_expand.begin(); it != m_expand.end(); ++it) {
      m_stack.push(*it);
    } // for loop

  } // if over node type //////////////////////////////////////////////

  return false; // default false

}


SearchNode* ParallelManager::nextNode() {

  SearchNode* n = NULL;
  if (m_stack.size()) {
    n = m_stack.top();
    m_stack.pop();
  }
  return n;

}


bool ParallelManager::applyAOBB(SearchNode* node, size_t countLimit) {
  assert(node);
  bool complete = false;

  this->resetLocalStack(node);
  // copy current node profiles and search stats
  vector<size_t> startNodeP = m_nodeProfile;
  vector<size_t> startLeafP = m_leafProfile;
  SearchStats startStats = m_space->stats;

  SearchNode* n = this->nextLeaf();
  SearchStats& currStats = m_space->stats;
  size_t countProc = 0;
  while (n) {
    m_prop.propagate(n, true, node);
    n = this->nextLeaf();
    countProc = currStats.numProcessed - startStats.numProcessed;
    if (countProc > countLimit)
      break;
  }

  // compute features
  SubprobFeatures* features = node->getSubprobFeatures();
  features->ratioPruned =
      (currStats.numPruned - startStats.numPruned) * 1.0 / countProc;
  features->ratioDead =
      (currStats.numDead - startStats.numDead) * 1.0 / countProc;
  features->ratioLeaf =
      (currStats.numLeaf - startStats.numLeaf) * 1.0 / countProc;

  features->avgNodeDepth = computeAvgDepth(startNodeP, m_nodeProfile, node->getDepth());
  features->avgLeafDepth = computeAvgDepth(startLeafP, m_leafProfile, node->getDepth());
  features->avgBranchDeg = pow(countProc, 1.0 / features->avgLeafDepth);

  /*
  size_t sumNodeP = 0;
  for (int d = node->getDepth(); d < m_nodeProfile.size(); ++d)
    sumNodeP += m_nodeProfile[d] - startNodeP[d];

  cout << "processed " << countProc << " sumNodeProf " << sumNodeP
       << " avgBra " << features->avgBranchDeg << endl;
  */

  complete = (n == NULL);
  node->clearChildren();
  DIAG(oss ss; ss << "Subproblem " << *node << " lookahead with " << countProc << " expansions, "
       << ((complete) ? "complete" : "not complete") << endl; myprint(ss.str());)
  return complete;
}


bool ParallelManager::applyLDS(SearchNode* node) {
  assert(node);
  PseudotreeNode* ptnode = m_pseudotree->getNode(node->getVar());
  if (ptnode->getSubWidth() > m_options->ibound) {
    return false;  // cannot be solved by LDS
  }

  m_ldsSearch->reset(node);
  SearchNode* n = m_ldsSearch->nextLeaf();
  while (n) {
    m_ldsProp->propagate(n,true,node);
    n = m_ldsSearch->nextLeaf();
  }
  DIAG(oss ss; ss << "Subproblem " << *node << " solved by LDS" << endl; myprint(ss.str());)
  return true;
}


string ParallelManager::filename(const char* pre, const char* ext, int count) const {
  ostringstream ss;
  ss << pre;
  ss << m_options->problemName;
  ss << ".";
  ss << m_options->runTag;
  if (count!=NONE) {
    ss << ".";
    ss << count;
  }
  ss << ext;

  return ss.str();
}


double ParallelManager::computeAvgDepth(const vector<size_t>& before, const vector<size_t>& after, int offset) {
  size_t leafCount = 0;
  for (size_t d = offset; d < before.size(); ++d)
    leafCount += after[d] - before[d];
  double avg = 0.0;
  for (size_t d = offset; d < before.size(); ++d)
    avg += (after[d] - before[d]) * (d - offset) * 1.0 / leafCount;
  return avg;
}


ParallelManager::ParallelManager(Problem* prob, Pseudotree* pt, SearchSpace* space, Heuristic* heur)
  : Search(prob, pt, space, heur), m_subprobCount(0)
//    , m_ldsSpace(NULL)
    , m_prop(prob, space)
{
#ifndef NO_CACHING
  // Init context cache table
  if (!m_space->cache)
    m_space->cache = new CacheTable(prob->getN());
#endif

  m_options = space->options;

  SearchNode* first = this->initSearch();
  assert(first);
  m_external.push_back(first);

  // set up LDS
//  m_ldsSpace = new SearchSpace(m_pseudotree, m_options);
//  m_ldsSpace->root = m_space->root;
  m_ldsSearch.reset(new LimitedDiscrepancy(prob, pt, m_space, heur, 0));
  m_ldsProp.reset(new BoundPropagator(prob, m_space, false));  // important: no caching

  // Set up subproblem sampler and complexity prediction
  m_sampleSpace.reset(new SearchSpace(pt, m_options));
  m_sampleSearch.reset(new BranchAndBoundSampler(prob, pt, m_sampleSpace.get(), heur));
  m_learner.reset(new LinearRegressionLearner(m_options));

}

#endif /* PARALLEL_STATIC */

