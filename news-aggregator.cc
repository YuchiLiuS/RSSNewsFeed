/**
 * File: news-aggregator.cc
 * ------------------------
 * When fully implements, pulls and parses every single
 * news article reachable from some RSS feed in the user-supplied
 * RSS News Feed XML file, and then allows the user to query the
 * index.
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include <memory>
#include <libxml/parser.h>
#include <libxml/catalog.h>
#include "ostreamlock.h"
#include "semaphore.h"
#include "article.h"
#include "rss-feed-list.h"
#include "rss-feed.h"
#include "rss-index.h"
#include "html-document.h"
#include "html-document-exception.h"
#include "rss-feed-exception.h"
#include "rss-feed-list-exception.h"
#include "news-aggregator-utils.h"
#include "string-utils.h"
using namespace std;

static const unsigned int kMaxfeed = 8;
static const unsigned int kMaxTperS= 12;
static const unsigned int kMaxthread=64;
static RSSIndex index;
static map<string, unique_ptr<semaphore>> serverlocks;
static semaphore feedsAllowed(kMaxfeed);
static semaphore threadsAllowed(kMaxthread);
static mutex rssindexlock;
static mutex servermaplock;

/**
 * Function: printUsage
 * --------------------
 * Prints usage information.  Should be invoked whenever the
 * user fails to provide a RSS feed name.
 */
static void printUsage(const string& executableName) {
  cerr << "Usage: " << executableName << " <feed-url>" << endl;
}
/*
 *Helper Function:printFeeds
 *Print out all the feeds in the feedList.
 */
/*static void printFeeds(const map<string, string>& feeds){
  int count=0;
  for(auto t: feeds){
    cout<<"["<<count++<<"]"<<"title: "<<t.second<<endl<<"url: "<<t.first<<endl;
  }
}
*/

/*
 *Method: articletoTokens
 *Pull the html file of the article and get the tokens in it.
 */
static void articletoTokens(const Article& article,unique_ptr<semaphore>& up){
  threadsAllowed.wait();//Thread limit of 64
  string title = article.title;
  if (shouldTruncate(title)) title = truncate(title);
  string url = article.url;
  HTMLDocument htmlDoc(url);
  if (shouldTruncate(url)) url = truncate(url);
  cout<<oslock<<"  " << setw(2) << setfill(' ')<<"Parsing \""<<title<<"\""<<endl<<osunlock;
  cout<<oslock<<"  " << setw(6) << setfill(' ')<<"[at \""<<url<<"\"]"<<endl<<osunlock;
  try {
    htmlDoc.parse();
  } catch (const HTMLDocumentException& htmle) {
    cerr << "Ran into trouble while pulling full html document from \""
	 << htmlDoc.getURL() << "\"." << endl; 
    cerr << "Aborting...." << endl;
    threadsAllowed.signal();
    up->signal();
    return;
 }
  const vector<string>& tokens = htmlDoc.getTokens();
  rssindexlock.lock();//lock index so no other thread change it
  index.add(article,tokens);
  rssindexlock.unlock();
  threadsAllowed.signal();
  up->signal();
}

/*
 *Method: feedtoTokens
 *Pull the articles from the feed.and call articletoTokens
 */

static void feedtoTokens(const pair<string,string>& feed){
  cout<<"Begin full download of feed URI: "<<feed.first<<endl;
  RSSFeed rssfeed(feed.first);
  try {
    rssfeed.parse();
  } catch (const RSSFeedException& rfe) {
    cerr << "Ran into trouble while pulling full RSS feed from \""
	 << feed.first << "\"." << endl; 
    cerr << "Aborting...." << endl;
    feedsAllowed.signal();
    return;
  }
  feedsAllowed.signal();
  vector<thread> articlethreads;
  const vector<Article>& articles = rssfeed.getArticles();
  for(const Article& article: articles){
    string serverurl=getURLServer(article.url);
    servermaplock.lock();
    unique_ptr<semaphore>& up=serverlocks[serverurl];
    if(up==nullptr){
      up.reset(new semaphore(kMaxTperS));//Create a semaphore for the url
    } 
    servermaplock.unlock();
    up->wait(); 
    articlethreads.push_back(thread(articletoTokens,article,ref(up)));
  }
  for (thread& t: articlethreads) t.join();
  cout<<oslock<<"End full download of feed URI: "<<feed.first<<endl<<osunlock;
  
} 


static void processAllFeeds(const string& feedListURI) {
  vector<thread> feedthreads;
  RSSFeedList feedList(feedListURI);
  try {
    feedList.parse();//Pulls the content from the encapsulated URL
  } catch (const RSSFeedListException& rfle) {
    cerr << "Ran into trouble while pulling full RSS feed list from \""
	 << feedListURI << "\"." << endl; 
    cerr << "Aborting...." << endl;
    exit(0);
  }
  auto allfeeds=feedList.getFeeds();
  //printFeeds(allfeeds);
  for(auto feed: allfeeds){
    feedsAllowed.wait();
    feedthreads.push_back(thread(feedtoTokens,feed));
  }
  for (thread& t: feedthreads) t.join();
  // add well-decomposed code to read all of the RSS news feeds from feedList
  // for their news articles, and for each news article URL, process it
  // as an HTMLDocument and add all of the tokens to the master RSSIndex.
}

/**
 * Function: queryIndex
 * --------------------
 * queryIndex repeatedly prompts the user for search terms, and
 * for each nonempty search term returns the list of matching documents,
 * ranked by frequency.
 */

static const size_t kMaxMatchesToShow = 15;
static void queryIndex() {
  while (true) {
    cout << "Enter a search term [or just hit <enter> to quit]: ";
    string response;
    getline(cin, response);
    response = trim(response);
    if (response.empty()) break;
    const vector<pair<Article, int> >& matches = index.getMatchingArticles(response);
    if (matches.empty()) {
      cout << "Ah, we didn't find the term \"" << response << "\". Try again." << endl;
    } else {
      cout << "That term appears in " << matches.size() << " article" 
	   << (matches.size() == 1 ? "" : "s") << ".  ";
      if (matches.size() > kMaxMatchesToShow) 
	cout << "Here are the top " << kMaxMatchesToShow << " of them:" << endl;
      else 
	cout << "Here they are:" << endl;
      size_t count = 0;
      for (const pair<Article, int>& match: matches) {
	if (count == kMaxMatchesToShow) break;
	count++;
	string title = match.first.title;
	if (shouldTruncate(title)) title = truncate(title);
	string url = match.first.url;
	if (shouldTruncate(url)) url = truncate(url);
	string times = match.second == 1 ? "time" : "times";
	cout << "  " << setw(2) << setfill(' ') << count << ".) "
	     << "\"" << title << "\" [appears " << match.second << " " << times << "]." << endl;
	cout << "       \"" << url << "\"" << endl;
      }
    }
  }
}

/**
 * Function: main
 * --------------
 * Defines the entry point into the entire executable.
 */

int main(int argc, const char *argv[]) {
  if (argc != 2) {
    cerr << "Error: wrong number of arguments." << endl;
    printUsage(argv[0]);
    exit(0);
  }
  
  string rssFeedListURI = argv[1];
  xmlInitParser();
  xmlInitializeCatalog();
  processAllFeeds(rssFeedListURI);
  xmlCatalogCleanup();
  xmlCleanupParser();
  cout << endl;
  queryIndex();
  cout << "Exiting...." << endl;
  return 0;
}
