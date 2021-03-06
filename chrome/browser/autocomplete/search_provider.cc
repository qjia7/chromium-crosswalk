// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/autocomplete/search_provider.h"

#include <algorithm>
#include <cmath>

#include "base/callback.h"
#include "base/i18n/break_iterator.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/icu_string_conversions.h"
#include "base/json/json_string_value_serializer.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram.h"
#include "base/prefs/pref_service.h"
#include "base/strings/string16.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/autocomplete/autocomplete_classifier.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/autocomplete/autocomplete_match.h"
#include "chrome/browser/autocomplete/autocomplete_provider_listener.h"
#include "chrome/browser/autocomplete/autocomplete_result.h"
#include "chrome/browser/autocomplete/keyword_provider.h"
#include "chrome/browser/autocomplete/url_prefix.h"
#include "chrome/browser/google/google_util.h"
#include "chrome/browser/history/history_service.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/history/in_memory_database.h"
#include "chrome/browser/metrics/variations/variations_http_header_provider.h"
#include "chrome/browser/omnibox/omnibox_field_trial.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/search_engines/template_url_prepopulate_data.h"
#include "chrome/browser/search_engines/template_url_service.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_instant_controller.h"
#include "chrome/browser/ui/search/instant_controller.h"
#include "chrome/common/net/url_fixer_upper.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "content/public/browser/user_metrics.h"
#include "grit/generated_resources.h"
#include "net/base/escape.h"
#include "net/base/load_flags.h"
#include "net/base/net_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/url_fetcher.h"
#include "net/url_request/url_request_status.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/url_util.h"


// Helpers --------------------------------------------------------------------

namespace {

// We keep track in a histogram how many suggest requests we send, how
// many suggest requests we invalidate (e.g., due to a user typing
// another character), and how many replies we receive.
// *** ADD NEW ENUMS AFTER ALL PREVIOUSLY DEFINED ONES! ***
//     (excluding the end-of-list enum value)
// We do not want values of existing enums to change or else it screws
// up the statistics.
enum SuggestRequestsHistogramValue {
  REQUEST_SENT = 1,
  REQUEST_INVALIDATED,
  REPLY_RECEIVED,
  MAX_SUGGEST_REQUEST_HISTOGRAM_VALUE
};

// The verbatim score for an input which is not an URL.
const int kNonURLVerbatimRelevance = 1300;

// Increments the appropriate value in the histogram by one.
void LogOmniboxSuggestRequest(
    SuggestRequestsHistogramValue request_value) {
  UMA_HISTOGRAM_ENUMERATION("Omnibox.SuggestRequests", request_value,
                            MAX_SUGGEST_REQUEST_HISTOGRAM_VALUE);
}

bool HasMultipleWords(const base::string16& text) {
  base::i18n::BreakIterator i(text, base::i18n::BreakIterator::BREAK_WORD);
  bool found_word = false;
  if (i.Init()) {
    while (i.Advance()) {
      if (i.IsWord()) {
        if (found_word)
          return true;
        found_word = true;
      }
    }
  }
  return false;
}

AutocompleteMatchType::Type GetAutocompleteMatchType(const std::string& type) {
  if (type == "ENTITY")
    return AutocompleteMatchType::SEARCH_SUGGEST_ENTITY;
  if (type == "INFINITE")
    return AutocompleteMatchType::SEARCH_SUGGEST_INFINITE;
  if (type == "PERSONALIZED")
    return AutocompleteMatchType::SEARCH_SUGGEST_PERSONALIZED;
  if (type == "PROFILE")
    return AutocompleteMatchType::SEARCH_SUGGEST_PROFILE;
  return AutocompleteMatchType::SEARCH_SUGGEST;
}

}  // namespace


// SuggestionDeletionHandler -------------------------------------------------

// This class handles making requests to the server in order to delete
// personalized suggestions.
class SuggestionDeletionHandler : public net::URLFetcherDelegate {
 public:
  typedef base::Callback<void(bool, SuggestionDeletionHandler*)>
      DeletionCompletedCallback;

  SuggestionDeletionHandler(
      const std::string& deletion_url,
      Profile* profile,
      const DeletionCompletedCallback& callback);

  virtual ~SuggestionDeletionHandler();

 private:
  // net::URLFetcherDelegate:
  virtual void OnURLFetchComplete(const net::URLFetcher* source) OVERRIDE;

  scoped_ptr<net::URLFetcher> deletion_fetcher_;
  DeletionCompletedCallback callback_;

  DISALLOW_COPY_AND_ASSIGN(SuggestionDeletionHandler);
};


SuggestionDeletionHandler::SuggestionDeletionHandler(
    const std::string& deletion_url,
    Profile* profile,
    const DeletionCompletedCallback& callback) : callback_(callback) {
  GURL url(deletion_url);
  DCHECK(url.is_valid());

  deletion_fetcher_.reset(net::URLFetcher::Create(
      SearchProvider::kDeletionURLFetcherID,
      url,
      net::URLFetcher::GET,
      this));
  deletion_fetcher_->SetRequestContext(profile->GetRequestContext());
  deletion_fetcher_->Start();
};

SuggestionDeletionHandler::~SuggestionDeletionHandler() {
};

void SuggestionDeletionHandler::OnURLFetchComplete(
    const net::URLFetcher* source) {
  DCHECK(source == deletion_fetcher_.get());
  callback_.Run(
      source->GetStatus().is_success() && (source->GetResponseCode() == 200),
      this);
};


// SearchProvider::Providers --------------------------------------------------

SearchProvider::Providers::Providers(TemplateURLService* template_url_service)
    : template_url_service_(template_url_service) {
}

const TemplateURL* SearchProvider::Providers::GetDefaultProviderURL() const {
  return default_provider_.empty() ? NULL :
      template_url_service_->GetTemplateURLForKeyword(default_provider_);
}

const TemplateURL* SearchProvider::Providers::GetKeywordProviderURL() const {
  return keyword_provider_.empty() ? NULL :
      template_url_service_->GetTemplateURLForKeyword(keyword_provider_);
}


// SearchProvider::Result -----------------------------------------------------

SearchProvider::Result::Result(bool from_keyword_provider,
                               int relevance,
                               bool relevance_from_server)
    : from_keyword_provider_(from_keyword_provider),
      relevance_(relevance),
      relevance_from_server_(relevance_from_server) {
}

SearchProvider::Result::~Result() {
}


// SearchProvider::SuggestResult ----------------------------------------------

SearchProvider::SuggestResult::SuggestResult(
    const base::string16& suggestion,
    AutocompleteMatchType::Type type,
    const base::string16& match_contents,
    const base::string16& annotation,
    const std::string& suggest_query_params,
    const std::string& deletion_url,
    bool from_keyword_provider,
    int relevance,
    bool relevance_from_server,
    bool should_prefetch,
    const base::string16& input_text)
    : Result(from_keyword_provider, relevance, relevance_from_server),
      suggestion_(suggestion),
      type_(type),
      annotation_(annotation),
      suggest_query_params_(suggest_query_params),
      deletion_url_(deletion_url),
      should_prefetch_(should_prefetch) {
  match_contents_ = match_contents;
  DCHECK(!match_contents_.empty());
  ClassifyMatchContents(true, input_text);
}

SearchProvider::SuggestResult::~SuggestResult() {
}

void SearchProvider::SuggestResult::ClassifyMatchContents(
    const bool allow_bolding_all,
    const base::string16& input_text) {
  size_t input_position = match_contents_.find(input_text);
  if (!allow_bolding_all && (input_position == base::string16::npos)) {
    // Bail if the code below to update the bolding would bold the whole
    // string.  Note that the string may already be entirely bolded; if
    // so, leave it as is.
    return;
  }
  match_contents_class_.clear();
  // We do intra-string highlighting for suggestions - the suggested segment
  // will be highlighted, e.g. for input_text = "you" the suggestion may be
  // "youtube", so we'll bold the "tube" section: you*tube*.
  if (input_text != match_contents_) {
    if (input_position == base::string16::npos) {
      // The input text is not a substring of the query string, e.g. input
      // text is "slasdot" and the query string is "slashdot", so we bold the
      // whole thing.
      match_contents_class_.push_back(ACMatchClassification(
          0, ACMatchClassification::MATCH));
    } else {
      // We don't iterate over the string here annotating all matches because
      // it looks odd to have every occurrence of a substring that may be as
      // short as a single character highlighted in a query suggestion result,
      // e.g. for input text "s" and query string "southwest airlines", it
      // looks odd if both the first and last s are highlighted.
      if (input_position != 0) {
        match_contents_class_.push_back(ACMatchClassification(
            0, ACMatchClassification::MATCH));
      }
      match_contents_class_.push_back(
          ACMatchClassification(input_position, ACMatchClassification::NONE));
      size_t next_fragment_position = input_position + input_text.length();
      if (next_fragment_position < match_contents_.length()) {
        match_contents_class_.push_back(ACMatchClassification(
            next_fragment_position, ACMatchClassification::MATCH));
      }
    }
  } else {
    // Otherwise, match_contents_ is a verbatim (what-you-typed) match, either
    // for the default provider or a keyword search provider.
    match_contents_class_.push_back(ACMatchClassification(
        0, ACMatchClassification::NONE));
  }
}

bool SearchProvider::SuggestResult::IsInlineable(
    const base::string16& input) const {
  return StartsWith(suggestion_, input, false);
}

int SearchProvider::SuggestResult::CalculateRelevance(
    const AutocompleteInput& input,
    bool keyword_provider_requested) const {
  if (!from_keyword_provider_ && keyword_provider_requested)
    return 100;
  return ((input.type() == AutocompleteInput::URL) ? 300 : 600);
}


// SearchProvider::NavigationResult -------------------------------------------

SearchProvider::NavigationResult::NavigationResult(
    const AutocompleteProvider& provider,
    const GURL& url,
    const base::string16& description,
    bool from_keyword_provider,
    int relevance,
    bool relevance_from_server,
    const base::string16& input_text,
    const std::string& languages)
    : Result(from_keyword_provider, relevance, relevance_from_server),
      url_(url),
      formatted_url_(AutocompleteInput::FormattedStringWithEquivalentMeaning(
          url, provider.StringForURLDisplay(url, true, false))),
      description_(description) {
  DCHECK(url_.is_valid());
  CalculateAndClassifyMatchContents(true, input_text, languages);
}

SearchProvider::NavigationResult::~NavigationResult() {
}

void SearchProvider::NavigationResult::CalculateAndClassifyMatchContents(
    const bool allow_bolding_nothing,
    const base::string16& input_text,
    const std::string& languages) {
  // First look for the user's input inside the formatted url as it would be
  // without trimming the scheme, so we can find matches at the beginning of the
  // scheme.
  const URLPrefix* prefix =
      URLPrefix::BestURLPrefix(formatted_url_, input_text);
  size_t match_start = (prefix == NULL) ?
      formatted_url_.find(input_text) : prefix->prefix.length();
  bool trim_http = !AutocompleteInput::HasHTTPScheme(input_text) &&
      (!prefix || (match_start != 0));
  const net::FormatUrlTypes format_types =
      net::kFormatUrlOmitAll & ~(trim_http ? 0 : net::kFormatUrlOmitHTTP);

  base::string16 match_contents = net::FormatUrl(url_, languages, format_types,
      net::UnescapeRule::SPACES, NULL, NULL, &match_start);
  // If the first match in the untrimmed string was inside a scheme that we
  // trimmed, look for a subsequent match.
  if (match_start == base::string16::npos)
    match_start = match_contents.find(input_text);
  // Update |match_contents_| and |match_contents_class_| if it's allowed.
  if (allow_bolding_nothing || (match_start != base::string16::npos)) {
    match_contents_ = match_contents;
    // Safe if |match_start| is npos; also safe if the input is longer than the
    // remaining contents after |match_start|.
    AutocompleteMatch::ClassifyLocationInString(match_start,
        input_text.length(), match_contents_.length(),
        ACMatchClassification::URL, &match_contents_class_);
  }
}

bool SearchProvider::NavigationResult::IsInlineable(
    const base::string16& input) const {
  return URLPrefix::BestURLPrefix(formatted_url_, input) != NULL;
}

int SearchProvider::NavigationResult::CalculateRelevance(
    const AutocompleteInput& input,
    bool keyword_provider_requested) const {
  return (from_keyword_provider_ || !keyword_provider_requested) ? 800 : 150;
}


// SearchProvider::CompareScoredResults ---------------------------------------

class SearchProvider::CompareScoredResults {
 public:
  bool operator()(const Result& a, const Result& b) {
    // Sort in descending relevance order.
    return a.relevance() > b.relevance();
  }
};


// SearchProvider::Results ----------------------------------------------------

SearchProvider::Results::Results() : verbatim_relevance(-1) {
}

SearchProvider::Results::~Results() {
}

void SearchProvider::Results::Clear() {
  suggest_results.clear();
  navigation_results.clear();
  verbatim_relevance = -1;
  metadata.clear();
}

bool SearchProvider::Results::HasServerProvidedScores() const {
  if (verbatim_relevance >= 0)
    return true;

  // Right now either all results of one type will be server-scored or they will
  // all be locally scored, but in case we change this later, we'll just check
  // them all.
  for (SuggestResults::const_iterator i(suggest_results.begin());
       i != suggest_results.end(); ++i) {
    if (i->relevance_from_server())
      return true;
  }
  for (NavigationResults::const_iterator i(navigation_results.begin());
       i != navigation_results.end(); ++i) {
    if (i->relevance_from_server())
      return true;
  }

  return false;
}


// SearchProvider -------------------------------------------------------------

// static
const int SearchProvider::kDefaultProviderURLFetcherID = 1;
const int SearchProvider::kKeywordProviderURLFetcherID = 2;
const int SearchProvider::kDeletionURLFetcherID = 3;
int SearchProvider::kMinimumTimeBetweenSuggestQueriesMs = 100;
const char SearchProvider::kRelevanceFromServerKey[] = "relevance_from_server";
const char SearchProvider::kShouldPrefetchKey[] = "should_prefetch";
const char SearchProvider::kSuggestMetadataKey[] = "suggest_metadata";
const char SearchProvider::kDeletionUrlKey[] = "deletion_url";
const char SearchProvider::kTrue[] = "true";
const char SearchProvider::kFalse[] = "false";

SearchProvider::SearchProvider(AutocompleteProviderListener* listener,
                               Profile* profile)
    : AutocompleteProvider(listener, profile,
          AutocompleteProvider::TYPE_SEARCH),
      providers_(TemplateURLServiceFactory::GetForProfile(profile)),
      suggest_results_pending_(0),
      field_trial_triggered_(false),
      field_trial_triggered_in_session_(false) {
}

// static
AutocompleteMatch SearchProvider::CreateSearchSuggestion(
    AutocompleteProvider* autocomplete_provider,
    const AutocompleteInput& input,
    const base::string16& input_text,
    const SuggestResult& suggestion,
    const TemplateURL* template_url,
    int accepted_suggestion,
    int omnibox_start_margin,
    bool append_extra_query_params) {
  AutocompleteMatch match(autocomplete_provider, suggestion.relevance(), false,
                          suggestion.type());

  if (!template_url)
    return match;
  match.keyword = template_url->keyword();
  match.contents = suggestion.match_contents();
  match.contents_class = suggestion.match_contents_class();

  if (!suggestion.annotation().empty())
    match.description = suggestion.annotation();

  match.allowed_to_be_default_match =
      (input_text == suggestion.match_contents());

  // When the user forced a query, we need to make sure all the fill_into_edit
  // values preserve that property.  Otherwise, if the user starts editing a
  // suggestion, non-Search results will suddenly appear.
  if (input.type() == AutocompleteInput::FORCED_QUERY)
    match.fill_into_edit.assign(base::ASCIIToUTF16("?"));
  if (suggestion.from_keyword_provider())
    match.fill_into_edit.append(match.keyword + base::char16(' '));
  if (!input.prevent_inline_autocomplete() &&
      StartsWith(suggestion.suggestion(), input_text, false)) {
    match.inline_autocompletion =
        suggestion.suggestion().substr(input_text.length());
    match.allowed_to_be_default_match = true;
  }
  match.fill_into_edit.append(suggestion.suggestion());

  const TemplateURLRef& search_url = template_url->url_ref();
  DCHECK(search_url.SupportsReplacement());
  match.search_terms_args.reset(
      new TemplateURLRef::SearchTermsArgs(suggestion.suggestion()));
  match.search_terms_args->original_query = input_text;
  match.search_terms_args->accepted_suggestion = accepted_suggestion;
  match.search_terms_args->omnibox_start_margin = omnibox_start_margin;
  match.search_terms_args->suggest_query_params =
      suggestion.suggest_query_params();
  match.search_terms_args->append_extra_query_params =
      append_extra_query_params;
  // This is the destination URL sans assisted query stats.  This must be set
  // so the AutocompleteController can properly de-dupe; the controller will
  // eventually overwrite it before it reaches the user.
  match.destination_url =
      GURL(search_url.ReplaceSearchTerms(*match.search_terms_args.get()));

  // Search results don't look like URLs.
  match.transition = suggestion.from_keyword_provider() ?
      content::PAGE_TRANSITION_KEYWORD : content::PAGE_TRANSITION_GENERATED;

  return match;
}

// static
bool SearchProvider::ShouldPrefetch(const AutocompleteMatch& match) {
  return match.GetAdditionalInfo(kShouldPrefetchKey) == kTrue;
}

// static
std::string SearchProvider::GetSuggestMetadata(const AutocompleteMatch& match) {
  return match.GetAdditionalInfo(kSuggestMetadataKey);
}

void SearchProvider::AddProviderInfo(ProvidersInfo* provider_info) const {
  provider_info->push_back(metrics::OmniboxEventProto_ProviderInfo());
  metrics::OmniboxEventProto_ProviderInfo& new_entry = provider_info->back();
  new_entry.set_provider(AsOmniboxEventProviderType());
  new_entry.set_provider_done(done_);
  std::vector<uint32> field_trial_hashes;
  OmniboxFieldTrial::GetActiveSuggestFieldTrialHashes(&field_trial_hashes);
  for (size_t i = 0; i < field_trial_hashes.size(); ++i) {
    if (field_trial_triggered_)
      new_entry.mutable_field_trial_triggered()->Add(field_trial_hashes[i]);
    if (field_trial_triggered_in_session_) {
      new_entry.mutable_field_trial_triggered_in_session()->Add(
          field_trial_hashes[i]);
    }
  }
}

void SearchProvider::DeleteMatch(const AutocompleteMatch& match) {
  DCHECK(match.deletable);

  deletion_handlers_.push_back(new SuggestionDeletionHandler(
      match.GetAdditionalInfo(SearchProvider::kDeletionUrlKey),
      profile_,
      base::Bind(&SearchProvider::OnDeletionComplete, base::Unretained(this))));

  HistoryService* const history_service =
      HistoryServiceFactory::GetForProfile(profile_, Profile::EXPLICIT_ACCESS);
  TemplateURL* template_url = match.GetTemplateURL(profile_, false);
  // This may be NULL if the template corresponding to the keyword has been
  // deleted or there is no keyword set.
  if (template_url != NULL) {
    history_service->DeleteMatchingURLsForKeyword(template_url->id(),
                                                  match.contents);
  }

  // Immediately update the list of matches to show the match was deleted,
  // regardless of whether the server request actually succeeds.
  DeleteMatchFromMatches(match);
}

void SearchProvider::ResetSession() {
  field_trial_triggered_in_session_ = false;
}

SearchProvider::~SearchProvider() {
}

// static
void SearchProvider::RemoveStaleResults(const base::string16& input,
                                        int verbatim_relevance,
                                        SuggestResults* suggest_results,
                                        NavigationResults* navigation_results) {
  DCHECK_GE(verbatim_relevance, 0);
  // Keep pointers to the head of (the highest scoring elements of)
  // |suggest_results| and |navigation_results|.  Iterate down the lists
  // removing non-inlineable results in order of decreasing relevance
  // scores.  Stop when the highest scoring element among those remaining
  // is inlineable or the element is less than |verbatim_relevance|.
  // This allows non-inlineable lower-scoring results to remain
  // because (i) they are guaranteed to not be inlined and (ii)
  // letting them remain reduces visual jank.  For instance, as the
  // user types the mis-spelled query "fpobar" (for foobar), the
  // suggestion "foobar" will be suggested on every keystroke.  If the
  // SearchProvider always removes all non-inlineable results, the user will
  // see visual jitter/jank as the result disappears and re-appears moments
  // later as the suggest server returns results.
  SuggestResults::iterator sug_it = suggest_results->begin();
  NavigationResults::iterator nav_it = navigation_results->begin();
  while ((sug_it != suggest_results->end()) ||
         (nav_it != navigation_results->end())) {
    const int sug_rel =
        (sug_it != suggest_results->end()) ? sug_it->relevance() : -1;
    const int nav_rel =
        (nav_it != navigation_results->end()) ? nav_it->relevance() : -1;
    if (std::max(sug_rel, nav_rel) < verbatim_relevance)
      break;
    if (sug_rel > nav_rel) {
      // The current top result is a search suggestion.
      if (sug_it->IsInlineable(input))
        break;
      sug_it = suggest_results->erase(sug_it);
    } else if (sug_rel == nav_rel) {
      // Have both results and they're tied.
      const bool sug_inlineable = sug_it->IsInlineable(input);
      const bool nav_inlineable = nav_it->IsInlineable(input);
      if (!sug_inlineable)
        sug_it = suggest_results->erase(sug_it);
      if (!nav_inlineable)
        nav_it = navigation_results->erase(nav_it);
      if (sug_inlineable || nav_inlineable)
        break;
    } else {
      // The current top result is a navigational suggestion.
      if (nav_it->IsInlineable(input))
        break;
      nav_it = navigation_results->erase(nav_it);
    }
  }
}

void SearchProvider::UpdateMatchContentsClass(const base::string16& input_text,
                                              Results* results) {
  for (SuggestResults::iterator sug_it = results->suggest_results.begin();
       sug_it != results->suggest_results.end(); ++sug_it) {
    sug_it->ClassifyMatchContents(false, input_text);
  }
  const std::string languages(
      profile_->GetPrefs()->GetString(prefs::kAcceptLanguages));
  for (NavigationResults::iterator nav_it = results->navigation_results.begin();
       nav_it != results->navigation_results.end(); ++nav_it) {
    nav_it->CalculateAndClassifyMatchContents(false, input_text, languages);
  }
}

// static
int SearchProvider::CalculateRelevanceForKeywordVerbatim(
    AutocompleteInput::Type type,
    bool prefer_keyword) {
  // This function is responsible for scoring verbatim query matches
  // for non-extension keywords.  KeywordProvider::CalculateRelevance()
  // scores verbatim query matches for extension keywords, as well as
  // for keyword matches (i.e., suggestions of a keyword itself, not a
  // suggestion of a query on a keyword search engine).  These two
  // functions are currently in sync, but there's no reason we
  // couldn't decide in the future to score verbatim matches
  // differently for extension and non-extension keywords.  If you
  // make such a change, however, you should update this comment to
  // describe it, so it's clear why the functions diverge.
  if (prefer_keyword)
    return 1500;
  return (type == AutocompleteInput::QUERY) ? 1450 : 1100;
}

void SearchProvider::Start(const AutocompleteInput& input,
                           bool minimal_changes) {
  // Do our best to load the model as early as possible.  This will reduce
  // odds of having the model not ready when really needed (a non-empty input).
  TemplateURLService* model = providers_.template_url_service();
  DCHECK(model);
  model->Load();

  matches_.clear();
  field_trial_triggered_ = false;

  // Can't return search/suggest results for bogus input or without a profile.
  if (!profile_ || (input.type() == AutocompleteInput::INVALID)) {
    Stop(false);
    return;
  }

  keyword_input_ = input;
  const TemplateURL* keyword_provider =
      KeywordProvider::GetSubstitutingTemplateURLForInput(model,
                                                          &keyword_input_);
  if (keyword_provider == NULL)
    keyword_input_.Clear();
  else if (keyword_input_.text().empty())
    keyword_provider = NULL;

  const TemplateURL* default_provider = model->GetDefaultSearchProvider();
  if (default_provider && !default_provider->SupportsReplacement())
    default_provider = NULL;

  if (keyword_provider == default_provider)
    default_provider = NULL;  // No use in querying the same provider twice.

  if (!default_provider && !keyword_provider) {
    // No valid providers.
    Stop(false);
    return;
  }

  // If we're still running an old query but have since changed the query text
  // or the providers, abort the query.
  base::string16 default_provider_keyword(default_provider ?
      default_provider->keyword() : base::string16());
  base::string16 keyword_provider_keyword(keyword_provider ?
      keyword_provider->keyword() : base::string16());
  if (!minimal_changes ||
      !providers_.equal(default_provider_keyword, keyword_provider_keyword)) {
    // Cancel any in-flight suggest requests.
    if (!done_)
      Stop(false);
  }

  providers_.set(default_provider_keyword, keyword_provider_keyword);

  if (input.text().empty()) {
    // User typed "?" alone.  Give them a placeholder result indicating what
    // this syntax does.
    if (default_provider) {
      AutocompleteMatch match;
      match.provider = this;
      match.contents.assign(l10n_util::GetStringUTF16(IDS_EMPTY_KEYWORD_VALUE));
      match.contents_class.push_back(
          ACMatchClassification(0, ACMatchClassification::NONE));
      match.keyword = providers_.default_provider();
      match.allowed_to_be_default_match = true;
      matches_.push_back(match);
    }
    Stop(false);
    return;
  }

  input_ = input;

  DoHistoryQuery(minimal_changes);
  StartOrStopSuggestQuery(minimal_changes);
  UpdateMatches();
}

void SearchProvider::Stop(bool clear_cached_results) {
  StopSuggest();
  done_ = true;

  if (clear_cached_results)
    ClearAllResults();
}

void SearchProvider::OnURLFetchComplete(const net::URLFetcher* source) {
  DCHECK(!done_);
  suggest_results_pending_--;
  LogOmniboxSuggestRequest(REPLY_RECEIVED);
  DCHECK_GE(suggest_results_pending_, 0);  // Should never go negative.

  const bool is_keyword = (source == keyword_fetcher_.get());
  // Ensure the request succeeded and that the provider used is still available.
  // A verbatim match cannot be generated without this provider, causing errors.
  const bool request_succeeded =
      source->GetStatus().is_success() && (source->GetResponseCode() == 200) &&
      (is_keyword ?
          providers_.GetKeywordProviderURL() :
          providers_.GetDefaultProviderURL());

  // Record response time for suggest requests sent to Google.  We care
  // only about the common case: the Google default provider used in
  // non-keyword mode.
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  if (!is_keyword && default_url &&
      (TemplateURLPrepopulateData::GetEngineType(*default_url) ==
       SEARCH_ENGINE_GOOGLE)) {
    const base::TimeDelta elapsed_time =
        base::TimeTicks::Now() - time_suggest_request_sent_;
    if (request_succeeded) {
      UMA_HISTOGRAM_TIMES("Omnibox.SuggestRequest.Success.GoogleResponseTime",
                          elapsed_time);
    } else {
      UMA_HISTOGRAM_TIMES("Omnibox.SuggestRequest.Failure.GoogleResponseTime",
                          elapsed_time);
    }
  }

  bool results_updated = false;
  if (request_succeeded) {
    const net::HttpResponseHeaders* const response_headers =
        source->GetResponseHeaders();
    std::string json_data;
    source->GetResponseAsString(&json_data);

    // JSON is supposed to be UTF-8, but some suggest service providers send
    // JSON files in non-UTF-8 encodings.  The actual encoding is usually
    // specified in the Content-Type header field.
    if (response_headers) {
      std::string charset;
      if (response_headers->GetCharset(&charset)) {
        base::string16 data_16;
        // TODO(jungshik): Switch to CodePageToUTF8 after it's added.
        if (base::CodepageToUTF16(json_data, charset.c_str(),
                                  base::OnStringConversionError::FAIL,
                                  &data_16))
          json_data = base::UTF16ToUTF8(data_16);
      }
    }

    scoped_ptr<base::Value> data(DeserializeJsonData(json_data));
    results_updated = data.get() && ParseSuggestResults(data.get(), is_keyword);
  }

  UpdateMatches();
  if (done_ || results_updated)
    listener_->OnProviderUpdate(results_updated);
}

void SearchProvider::OnDeletionComplete(bool success,
                                        SuggestionDeletionHandler* handler) {
  RecordDeletionResult(success);
  SuggestionDeletionHandlers::iterator it = std::find(
      deletion_handlers_.begin(), deletion_handlers_.end(), handler);
  DCHECK(it != deletion_handlers_.end());
  deletion_handlers_.erase(it);
}


void SearchProvider::RecordDeletionResult(bool success) {
  if (success) {
    content::RecordAction(
        base::UserMetricsAction("Omnibox.ServerSuggestDelete.Success"));
  } else {
    content::RecordAction(
        base::UserMetricsAction("Omnibox.ServerSuggestDelete.Failure"));
  }
}

void SearchProvider::DeleteMatchFromMatches(const AutocompleteMatch& match) {
  for (ACMatches::iterator i(matches_.begin()); i != matches_.end(); ++i) {
    // Find the desired match to delete by checking the type and contents.
    // We can't check the destination URL, because the autocomplete controller
    // may have reformulated that. Not that while checking for matching
    // contents works for personalized suggestions, if more match types gain
    // deletion support, this algorithm may need to be re-examined.
    if (i->contents == match.contents && i->type == match.type) {
      matches_.erase(i);
      break;
    }
  }
  listener_->OnProviderUpdate(true);
}

void SearchProvider::Run() {
  // Start a new request with the current input.
  suggest_results_pending_ = 0;
  time_suggest_request_sent_ = base::TimeTicks::Now();

  default_fetcher_.reset(CreateSuggestFetcher(kDefaultProviderURLFetcherID,
      providers_.GetDefaultProviderURL(), input_));
  keyword_fetcher_.reset(CreateSuggestFetcher(kKeywordProviderURLFetcherID,
      providers_.GetKeywordProviderURL(), keyword_input_));

  // Both the above can fail if the providers have been modified or deleted
  // since the query began.
  if (suggest_results_pending_ == 0) {
    UpdateDone();
    // We only need to update the listener if we're actually done.
    if (done_)
      listener_->OnProviderUpdate(false);
  }
}

void SearchProvider::DoHistoryQuery(bool minimal_changes) {
  // The history query results are synchronous, so if minimal_changes is true,
  // we still have the last results and don't need to do anything.
  if (minimal_changes)
    return;

  base::TimeTicks do_history_query_start_time(base::TimeTicks::Now());

  keyword_history_results_.clear();
  default_history_results_.clear();

  if (OmniboxFieldTrial::SearchHistoryDisable(
      input_.current_page_classification()))
    return;

  base::TimeTicks start_time(base::TimeTicks::Now());
  HistoryService* const history_service =
      HistoryServiceFactory::GetForProfile(profile_, Profile::EXPLICIT_ACCESS);
  base::TimeTicks now(base::TimeTicks::Now());
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.GetHistoryServiceTime",
                      now - start_time);
  start_time = now;
  history::URLDatabase* url_db = history_service ?
      history_service->InMemoryDatabase() : NULL;
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.InMemoryDatabaseTime",
                      base::TimeTicks::Now() - start_time);
  if (!url_db)
    return;

  // Request history for both the keyword and default provider.  We grab many
  // more matches than we'll ultimately clamp to so that if there are several
  // recent multi-word matches who scores are lowered (see
  // AddHistoryResultsToMap()), they won't crowd out older, higher-scoring
  // matches.  Note that this doesn't fix the problem entirely, but merely
  // limits it to cases with a very large number of such multi-word matches; for
  // now, this seems OK compared with the complexity of a real fix, which would
  // require multiple searches and tracking of "single- vs. multi-word" in the
  // database.
  int num_matches = kMaxMatches * 5;
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  if (default_url) {
    start_time = base::TimeTicks::Now();
    url_db->GetMostRecentKeywordSearchTerms(default_url->id(), input_.text(),
        num_matches, &default_history_results_);
    UMA_HISTOGRAM_TIMES(
        "Omnibox.SearchProvider.GetMostRecentKeywordTermsDefaultProviderTime",
        base::TimeTicks::Now() - start_time);
  }
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if (keyword_url) {
    url_db->GetMostRecentKeywordSearchTerms(keyword_url->id(),
        keyword_input_.text(), num_matches, &keyword_history_results_);
  }
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.DoHistoryQueryTime",
                      base::TimeTicks::Now() - do_history_query_start_time);
}

void SearchProvider::StartOrStopSuggestQuery(bool minimal_changes) {
  if (!IsQuerySuitableForSuggest()) {
    StopSuggest();
    ClearAllResults();
    return;
  }

  // For the minimal_changes case, if we finished the previous query and still
  // have its results, or are allowed to keep running it, just do that, rather
  // than starting a new query.
  if (minimal_changes &&
      (!default_results_.suggest_results.empty() ||
       !default_results_.navigation_results.empty() ||
       !keyword_results_.suggest_results.empty() ||
       !keyword_results_.navigation_results.empty() ||
       (!done_ &&
        input_.matches_requested() == AutocompleteInput::ALL_MATCHES)))
    return;

  // We can't keep running any previous query, so halt it.
  StopSuggest();

  // Remove existing results that cannot inline autocomplete the new input.
  RemoveAllStaleResults();

  // Update the content classifications of remaining results so they look good
  // against the current input.
  UpdateMatchContentsClass(input_.text(), &default_results_);
  if (!keyword_input_.text().empty())
    UpdateMatchContentsClass(keyword_input_.text(), &keyword_results_);

  // We can't start a new query if we're only allowed synchronous results.
  if (input_.matches_requested() != AutocompleteInput::ALL_MATCHES)
    return;

  // To avoid flooding the suggest server, don't send a query until at
  // least 100 ms since the last query.
  base::TimeTicks next_suggest_time(time_suggest_request_sent_ +
      base::TimeDelta::FromMilliseconds(kMinimumTimeBetweenSuggestQueriesMs));
  base::TimeTicks now(base::TimeTicks::Now());
  if (now >= next_suggest_time) {
    Run();
    return;
  }
  timer_.Start(FROM_HERE, next_suggest_time - now, this, &SearchProvider::Run);
}

bool SearchProvider::IsQuerySuitableForSuggest() const {
  // Don't run Suggest in incognito mode, if the engine doesn't support it, or
  // if the user has disabled it.
  const TemplateURL* default_url = providers_.GetDefaultProviderURL();
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if (profile_->IsOffTheRecord() ||
      ((!default_url || default_url->suggestions_url().empty()) &&
       (!keyword_url || keyword_url->suggestions_url().empty())) ||
      !profile_->GetPrefs()->GetBoolean(prefs::kSearchSuggestEnabled))
    return false;

  // If the input type might be a URL, we take extra care so that private data
  // isn't sent to the server.

  // FORCED_QUERY means the user is explicitly asking us to search for this, so
  // we assume it isn't a URL and/or there isn't private data.
  if (input_.type() == AutocompleteInput::FORCED_QUERY)
    return true;

  // Next we check the scheme.  If this is UNKNOWN/URL with a scheme that isn't
  // http/https/ftp, we shouldn't send it.  Sending things like file: and data:
  // is both a waste of time and a disclosure of potentially private, local
  // data.  Other "schemes" may actually be usernames, and we don't want to send
  // passwords.  If the scheme is OK, we still need to check other cases below.
  // If this is QUERY, then the presence of these schemes means the user
  // explicitly typed one, and thus this is probably a URL that's being entered
  // and happens to currently be invalid -- in which case we again want to run
  // our checks below.  Other QUERY cases are less likely to be URLs and thus we
  // assume we're OK.
  if (!LowerCaseEqualsASCII(input_.scheme(), content::kHttpScheme) &&
      !LowerCaseEqualsASCII(input_.scheme(), content::kHttpsScheme) &&
      !LowerCaseEqualsASCII(input_.scheme(), content::kFtpScheme))
    return (input_.type() == AutocompleteInput::QUERY);

  // Don't send URLs with usernames, queries or refs.  Some of these are
  // private, and the Suggest server is unlikely to have any useful results
  // for any of them.  Also don't send URLs with ports, as we may initially
  // think that a username + password is a host + port (and we don't want to
  // send usernames/passwords), and even if the port really is a port, the
  // server is once again unlikely to have and useful results.
  // Note that we only block based on refs if the input is URL-typed, as search
  // queries can legitimately have #s in them which the URL parser
  // overaggressively categorizes as a url with a ref.
  const url_parse::Parsed& parts = input_.parts();
  if (parts.username.is_nonempty() || parts.port.is_nonempty() ||
      parts.query.is_nonempty() ||
      (parts.ref.is_nonempty() && (input_.type() == AutocompleteInput::URL)))
    return false;

  // Don't send anything for https except the hostname.  Hostnames are OK
  // because they are visible when the TCP connection is established, but the
  // specific path may reveal private information.
  if (LowerCaseEqualsASCII(input_.scheme(), content::kHttpsScheme) &&
      parts.path.is_nonempty())
    return false;

  return true;
}

void SearchProvider::StopSuggest() {
  // Increment the appropriate field in the histogram by the number of
  // pending requests that were invalidated.
  for (int i = 0; i < suggest_results_pending_; i++)
    LogOmniboxSuggestRequest(REQUEST_INVALIDATED);
  suggest_results_pending_ = 0;
  timer_.Stop();
  // Stop any in-progress URL fetches.
  keyword_fetcher_.reset();
  default_fetcher_.reset();
}

void SearchProvider::ClearAllResults() {
  keyword_results_.Clear();
  default_results_.Clear();
}

void SearchProvider::RemoveAllStaleResults() {
  // We only need to remove stale results (which ensures the top-scoring
  // match is inlineable) if the user is not in reorder mode.  In reorder
  // mode, the autocomplete system will reorder results to make sure the
  // top result is inlineable.
  const bool omnibox_will_reorder_for_legal_default_match =
      OmniboxFieldTrial::ReorderForLegalDefaultMatch(
          input_.current_page_classification());
  // In theory it would be better to run an algorithm like that in
  // RemoveStaleResults(...) below that uses all four results lists
  // and both verbatim scores at once.  However, that will be much
  // more complicated for little obvious gain.  For code simplicity
  // and ease in reasoning about the invariants involved, this code
  // removes stales results from the keyword provider and default
  // provider independently.
  if (!omnibox_will_reorder_for_legal_default_match) {
    RemoveStaleResults(input_.text(), GetVerbatimRelevance(NULL),
                       &default_results_.suggest_results,
                       &default_results_.navigation_results);
    if (!keyword_input_.text().empty()) {
      RemoveStaleResults(keyword_input_.text(),
                         GetKeywordVerbatimRelevance(NULL),
                         &keyword_results_.suggest_results,
                         &keyword_results_.navigation_results);
    }
  }
  if (keyword_input_.text().empty()) {
    // User is either in keyword mode with a blank input or out of
    // keyword mode entirely.
    keyword_results_.Clear();
  }
}

void SearchProvider::ApplyCalculatedRelevance() {
  ApplyCalculatedSuggestRelevance(&keyword_results_.suggest_results);
  ApplyCalculatedSuggestRelevance(&default_results_.suggest_results);
  ApplyCalculatedNavigationRelevance(&keyword_results_.navigation_results);
  ApplyCalculatedNavigationRelevance(&default_results_.navigation_results);
  default_results_.verbatim_relevance = -1;
  keyword_results_.verbatim_relevance = -1;
}

void SearchProvider::ApplyCalculatedSuggestRelevance(SuggestResults* list) {
  for (size_t i = 0; i < list->size(); ++i) {
    SuggestResult& result = (*list)[i];
    result.set_relevance(
        result.CalculateRelevance(input_, providers_.has_keyword_provider()) +
        (list->size() - i - 1));
    result.set_relevance_from_server(false);
  }
}

void SearchProvider::ApplyCalculatedNavigationRelevance(
    NavigationResults* list) {
  for (size_t i = 0; i < list->size(); ++i) {
    NavigationResult& result = (*list)[i];
    result.set_relevance(
        result.CalculateRelevance(input_, providers_.has_keyword_provider()) +
        (list->size() - i - 1));
    result.set_relevance_from_server(false);
  }
}

net::URLFetcher* SearchProvider::CreateSuggestFetcher(
    int id,
    const TemplateURL* template_url,
    const AutocompleteInput& input) {
  if (!template_url || template_url->suggestions_url().empty())
    return NULL;

  // Bail if the suggestion URL is invalid with the given replacements.
  TemplateURLRef::SearchTermsArgs search_term_args(input.text());
  search_term_args.cursor_position = input.cursor_position();
  search_term_args.page_classification = input.current_page_classification();
  GURL suggest_url(template_url->suggestions_url_ref().ReplaceSearchTerms(
      search_term_args));
  if (!suggest_url.is_valid())
    return NULL;
  // Send the current page URL if user setting and URL requirements are met and
  // the user is in the field trial.
  if (CanSendURL(current_page_url_, suggest_url, template_url,
                 input.current_page_classification(), profile_) &&
      OmniboxFieldTrial::InZeroSuggestAfterTypingFieldTrial()) {
    search_term_args.current_page_url = current_page_url_.spec();
    // Create the suggest URL again with the current page URL.
    suggest_url = GURL(template_url->suggestions_url_ref().ReplaceSearchTerms(
        search_term_args));
  }

  suggest_results_pending_++;
  LogOmniboxSuggestRequest(REQUEST_SENT);

  net::URLFetcher* fetcher =
      net::URLFetcher::Create(id, suggest_url, net::URLFetcher::GET, this);
  fetcher->SetRequestContext(profile_->GetRequestContext());
  fetcher->SetLoadFlags(net::LOAD_DO_NOT_SAVE_COOKIES);
  // Add Chrome experiment state to the request headers.
  net::HttpRequestHeaders headers;
  chrome_variations::VariationsHttpHeaderProvider::GetInstance()->AppendHeaders(
      fetcher->GetOriginalURL(), profile_->IsOffTheRecord(), false, &headers);
  fetcher->SetExtraRequestHeaders(headers.ToString());
  fetcher->Start();
  return fetcher;
}

scoped_ptr<base::Value> SearchProvider::DeserializeJsonData(
    std::string json_data) {
  // The JSON response should be an array.
  for (size_t response_start_index = json_data.find("["), i = 0;
       response_start_index != std::string::npos && i < 5;
       response_start_index = json_data.find("[", 1), i++) {
    // Remove any XSSI guards to allow for JSON parsing.
    if (response_start_index > 0)
      json_data.erase(0, response_start_index);

    JSONStringValueSerializer deserializer(json_data);
    deserializer.set_allow_trailing_comma(true);
    int error_code = 0;
    scoped_ptr<base::Value> data(deserializer.Deserialize(&error_code, NULL));
    if (error_code == 0)
      return data.Pass();
  }
  return scoped_ptr<base::Value>();
}

bool SearchProvider::ParseSuggestResults(base::Value* root_val,
                                         bool is_keyword) {
  base::string16 query;
  base::ListValue* root_list = NULL;
  base::ListValue* results_list = NULL;
  const base::string16& input_text =
      is_keyword ? keyword_input_.text() : input_.text();
  if (!root_val->GetAsList(&root_list) || !root_list->GetString(0, &query) ||
      (query != input_text) || !root_list->GetList(1, &results_list))
    return false;

  // 3rd element: Description list.
  base::ListValue* descriptions = NULL;
  root_list->GetList(2, &descriptions);

  // 4th element: Disregard the query URL list for now.

  // Reset suggested relevance information from the default provider.
  Results* results = is_keyword ? &keyword_results_ : &default_results_;
  results->verbatim_relevance = -1;

  // 5th element: Optional key-value pairs from the Suggest server.
  base::ListValue* types = NULL;
  base::ListValue* relevances = NULL;
  base::ListValue* suggestion_details = NULL;
  base::DictionaryValue* extras = NULL;
  int prefetch_index = -1;
  if (root_list->GetDictionary(4, &extras)) {
    extras->GetList("google:suggesttype", &types);

    // Discard this list if its size does not match that of the suggestions.
    if (extras->GetList("google:suggestrelevance", &relevances) &&
        (relevances->GetSize() != results_list->GetSize()))
      relevances = NULL;
    extras->GetInteger("google:verbatimrelevance",
                       &results->verbatim_relevance);

    // Check if the active suggest field trial (if any) has triggered either
    // for the default provider or keyword provider.
    bool triggered = false;
    extras->GetBoolean("google:fieldtrialtriggered", &triggered);
    field_trial_triggered_ |= triggered;
    field_trial_triggered_in_session_ |= triggered;

    base::DictionaryValue* client_data = NULL;
    if (extras->GetDictionary("google:clientdata", &client_data) && client_data)
      client_data->GetInteger("phi", &prefetch_index);

    if (extras->GetList("google:suggestdetail", &suggestion_details) &&
        suggestion_details->GetSize() != results_list->GetSize())
      suggestion_details = NULL;

    // Store the metadata that came with the response in case we need to pass it
    // along with the prefetch query to Instant.
    JSONStringValueSerializer json_serializer(&results->metadata);
    json_serializer.Serialize(*extras);
  }

  // Clear the previous results now that new results are available.
  results->suggest_results.clear();
  results->navigation_results.clear();

  base::string16 suggestion;
  std::string type;
  int relevance = -1;
  // Prohibit navsuggest in FORCED_QUERY mode.  Users wants queries, not URLs.
  const bool allow_navsuggest =
      (is_keyword ? keyword_input_.type() : input_.type()) !=
      AutocompleteInput::FORCED_QUERY;
  const std::string languages(
      profile_->GetPrefs()->GetString(prefs::kAcceptLanguages));
  for (size_t index = 0; results_list->GetString(index, &suggestion); ++index) {
    // Google search may return empty suggestions for weird input characters,
    // they make no sense at all and can cause problems in our code.
    if (suggestion.empty())
      continue;

    // Apply valid suggested relevance scores; discard invalid lists.
    if (relevances != NULL && !relevances->GetInteger(index, &relevance))
      relevances = NULL;
    if (types && types->GetString(index, &type) && (type == "NAVIGATION")) {
      // Do not blindly trust the URL coming from the server to be valid.
      GURL url(URLFixerUpper::FixupURL(
          base::UTF16ToUTF8(suggestion), std::string()));
      if (url.is_valid() && allow_navsuggest) {
        base::string16 title;
        if (descriptions != NULL)
          descriptions->GetString(index, &title);
        results->navigation_results.push_back(NavigationResult(
            *this, url, title, is_keyword, relevance, true, input_text,
            languages));
      }
    } else {
      AutocompleteMatchType::Type match_type = GetAutocompleteMatchType(type);
      bool should_prefetch = static_cast<int>(index) == prefetch_index;
      base::DictionaryValue* suggestion_detail = NULL;
      base::string16 match_contents = suggestion;
      base::string16 annotation;
      std::string suggest_query_params;
      std::string deletion_url;

      if (suggestion_details) {
        suggestion_details->GetDictionary(index, &suggestion_detail);
        if (suggestion_detail) {
          suggestion_detail->GetString("du", &deletion_url);
          suggestion_detail->GetString("title", &match_contents) ||
              suggestion_detail->GetString("t", &match_contents);
          // Error correction for bad data from server.
          if (match_contents.empty())
            match_contents = suggestion;
          suggestion_detail->GetString("annotation", &annotation) ||
              suggestion_detail->GetString("a", &annotation);
          suggestion_detail->GetString("query_params", &suggest_query_params) ||
              suggestion_detail->GetString("q", &suggest_query_params);
        }
      }

      // TODO(kochi): Improve calculator suggestion presentation.
      results->suggest_results.push_back(SuggestResult(
          suggestion, match_type, match_contents, annotation,
          suggest_query_params, deletion_url, is_keyword, relevance, true,
          should_prefetch, input_text));
    }
  }

  // Ignore suggested scores for non-keyword matches in keyword mode; if the
  // server is allowed to score these, it could interfere with the user's
  // ability to get good keyword results.
  const bool abandon_suggested_scores =
      !is_keyword && !providers_.keyword_provider().empty();
  // Apply calculated relevance scores to suggestions if a valid list was
  // not provided or we're abandoning suggested scores entirely.
  if ((relevances == NULL) || abandon_suggested_scores) {
    ApplyCalculatedSuggestRelevance(&results->suggest_results);
    ApplyCalculatedNavigationRelevance(&results->navigation_results);
    // If abandoning scores entirely, also abandon the verbatim score.
    if (abandon_suggested_scores)
      results->verbatim_relevance = -1;
  }

  // Keep the result lists sorted.
  const CompareScoredResults comparator = CompareScoredResults();
  std::stable_sort(results->suggest_results.begin(),
                   results->suggest_results.end(),
                   comparator);
  std::stable_sort(results->navigation_results.begin(),
                   results->navigation_results.end(),
                   comparator);
  return true;
}

void SearchProvider::ConvertResultsToAutocompleteMatches() {
  // Convert all the results to matches and add them to a map, so we can keep
  // the most relevant match for each result.
  base::TimeTicks start_time(base::TimeTicks::Now());
  MatchMap map;
  const base::Time no_time;
  int did_not_accept_keyword_suggestion =
      keyword_results_.suggest_results.empty() ?
      TemplateURLRef::NO_SUGGESTIONS_AVAILABLE :
      TemplateURLRef::NO_SUGGESTION_CHOSEN;

  bool relevance_from_server;
  int verbatim_relevance = GetVerbatimRelevance(&relevance_from_server);
  int did_not_accept_default_suggestion =
      default_results_.suggest_results.empty() ?
      TemplateURLRef::NO_SUGGESTIONS_AVAILABLE :
      TemplateURLRef::NO_SUGGESTION_CHOSEN;
  if (verbatim_relevance > 0) {
    SuggestResult verbatim(
        input_.text(), AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
        input_.text(), base::string16(), std::string(), std::string(), false,
        verbatim_relevance, relevance_from_server, false, input_.text());
    AddMatchToMap(verbatim, input_.text(), std::string(),
                  did_not_accept_default_suggestion, &map);
  }
  if (!keyword_input_.text().empty()) {
    const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
    // We only create the verbatim search query match for a keyword
    // if it's not an extension keyword.  Extension keywords are handled
    // in KeywordProvider::Start().  (Extensions are complicated...)
    // Note: in this provider, SEARCH_OTHER_ENGINE must correspond
    // to the keyword verbatim search query.  Do not create other matches
    // of type SEARCH_OTHER_ENGINE.
    if (keyword_url &&
        (keyword_url->GetType() != TemplateURL::OMNIBOX_API_EXTENSION)) {
      bool keyword_relevance_from_server;
      const int keyword_verbatim_relevance =
          GetKeywordVerbatimRelevance(&keyword_relevance_from_server);
      if (keyword_verbatim_relevance > 0) {
        SuggestResult verbatim(
            keyword_input_.text(), AutocompleteMatchType::SEARCH_OTHER_ENGINE,
            keyword_input_.text(), base::string16(), std::string(),
            std::string(), true, keyword_verbatim_relevance,
            keyword_relevance_from_server, false, keyword_input_.text());
        AddMatchToMap(verbatim, keyword_input_.text(), std::string(),
                      did_not_accept_keyword_suggestion, &map);
      }
    }
  }
  AddHistoryResultsToMap(keyword_history_results_, true,
                         did_not_accept_keyword_suggestion, &map);
  AddHistoryResultsToMap(default_history_results_, false,
                         did_not_accept_default_suggestion, &map);

  AddSuggestResultsToMap(keyword_results_.suggest_results,
                         keyword_results_.metadata, &map);
  AddSuggestResultsToMap(default_results_.suggest_results,
                         default_results_.metadata, &map);

  ACMatches matches;
  for (MatchMap::const_iterator i(map.begin()); i != map.end(); ++i)
    matches.push_back(i->second);

  AddNavigationResultsToMatches(keyword_results_.navigation_results, &matches);
  AddNavigationResultsToMatches(default_results_.navigation_results, &matches);

  // Now add the most relevant matches to |matches_|.  We take up to kMaxMatches
  // suggest/navsuggest matches, regardless of origin.  If Instant Extended is
  // enabled and we have server-provided (and thus hopefully more accurate)
  // scores for some suggestions, we allow more of those, until we reach
  // AutocompleteResult::kMaxMatches total matches (that is, enough to fill the
  // whole popup).
  //
  // We will always return any verbatim matches, no matter how we obtained their
  // scores, unless we have already accepted AutocompleteResult::kMaxMatches
  // higher-scoring matches under the conditions above.
  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "Omnibox.SearchProvider.NumMatchesToSort", matches.size(), 1, 50, 20);
  std::sort(matches.begin(), matches.end(), &AutocompleteMatch::MoreRelevant);
  matches_.clear();

  size_t num_suggestions = 0;
  for (ACMatches::const_iterator i(matches.begin());
       (i != matches.end()) &&
           (matches_.size() < AutocompleteResult::kMaxMatches);
       ++i) {
    // SEARCH_OTHER_ENGINE is only used in the SearchProvider for the keyword
    // verbatim result, so this condition basically means "if this match is a
    // suggestion of some sort".
    if ((i->type != AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED) &&
        (i->type != AutocompleteMatchType::SEARCH_OTHER_ENGINE)) {
      // If we've already hit the limit on non-server-scored suggestions, and
      // this isn't a server-scored suggestion we can add, skip it.
      if ((num_suggestions >= kMaxMatches) &&
          (!chrome::IsInstantExtendedAPIEnabled() ||
           (i->GetAdditionalInfo(kRelevanceFromServerKey) != kTrue))) {
        continue;
      }

      ++num_suggestions;
    }

    matches_.push_back(*i);
  }
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.ConvertResultsTime",
                      base::TimeTicks::Now() - start_time);
}

ACMatches::const_iterator SearchProvider::FindTopMatch(
    bool autocomplete_result_will_reorder_for_default_match) const {
  if (!autocomplete_result_will_reorder_for_default_match)
    return matches_.begin();
  ACMatches::const_iterator it = matches_.begin();
  while ((it != matches_.end()) && !it->allowed_to_be_default_match)
    ++it;
  return it;
}

bool SearchProvider::IsTopMatchNavigationInKeywordMode(
    bool autocomplete_result_will_reorder_for_default_match) const {
  ACMatches::const_iterator first_match =
      FindTopMatch(autocomplete_result_will_reorder_for_default_match);
  return !providers_.keyword_provider().empty() &&
      (first_match != matches_.end()) &&
      (first_match->type == AutocompleteMatchType::NAVSUGGEST);
}

bool SearchProvider::HasKeywordDefaultMatchInKeywordMode() const {
  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  // If the user is not in keyword mode, return true to say that this
  // constraint is not violated.
  if (keyword_url == NULL)
    return true;
  for (ACMatches::const_iterator it = matches_.begin(); it != matches_.end();
       ++it) {
    if ((it->keyword == keyword_url->keyword()) &&
        it->allowed_to_be_default_match)
      return true;
  }
  return false;
}

bool SearchProvider::IsTopMatchScoreTooLow(
    bool autocomplete_result_will_reorder_for_default_match) const {
  // In reorder mode, there's no such thing as a score that's too low.
  if (autocomplete_result_will_reorder_for_default_match)
    return false;

  // Here we use CalculateRelevanceForVerbatimIgnoringKeywordModeState()
  // rather than CalculateRelevanceForVerbatim() because the latter returns
  // a very low score (250) if keyword mode is active.  This is because
  // when keyword mode is active the user probably wants the keyword matches,
  // not matches from the default provider.  Hence, we use the version of
  // the function that ignores whether keyword mode is active.  This allows
  // SearchProvider to maintain its contract with the AutocompleteController
  // that it will always provide an inlineable match with a reasonable
  // score.
  return matches_.front().relevance <
      CalculateRelevanceForVerbatimIgnoringKeywordModeState();
}

bool SearchProvider::IsTopMatchSearchWithURLInput(
    bool autocomplete_result_will_reorder_for_default_match) const {
  ACMatches::const_iterator first_match =
      FindTopMatch(autocomplete_result_will_reorder_for_default_match);
  return (input_.type() == AutocompleteInput::URL) &&
      (first_match != matches_.end()) &&
      (first_match->relevance > CalculateRelevanceForVerbatim()) &&
      (first_match->type != AutocompleteMatchType::NAVSUGGEST);
}

bool SearchProvider::HasValidDefaultMatch(
    bool autocomplete_result_will_reorder_for_default_match) const {
  // One of the SearchProvider matches may need to be the overall default.  If
  // AutocompleteResult is allowed to reorder matches, this means we simply
  // need at least one match in the list to be |allowed_to_be_default_match|.
  // If no reordering is possible, however, then our first match needs to have
  // this flag.
  for (ACMatches::const_iterator it = matches_.begin(); it != matches_.end();
       ++it) {
    if (it->allowed_to_be_default_match)
      return true;
    if (!autocomplete_result_will_reorder_for_default_match)
      return false;
  }
  return false;
}

void SearchProvider::UpdateMatches() {
  base::TimeTicks update_matches_start_time(base::TimeTicks::Now());
  ConvertResultsToAutocompleteMatches();

  // Check constraints that may be violated by suggested relevances.
  if (!matches_.empty() &&
      (default_results_.HasServerProvidedScores() ||
       keyword_results_.HasServerProvidedScores())) {
    // These blocks attempt to repair undesirable behavior by suggested
    // relevances with minimal impact, preserving other suggested relevances.

    // True if the omnibox will reorder matches as necessary to make the first
    // one something that is allowed to be the default match.
    const bool omnibox_will_reorder_for_legal_default_match =
        OmniboxFieldTrial::ReorderForLegalDefaultMatch(
            input_.current_page_classification());
    if (IsTopMatchNavigationInKeywordMode(
        omnibox_will_reorder_for_legal_default_match)) {
      // Correct the suggested relevance scores if the top match is a
      // navigation in keyword mode, since inlining a navigation match
      // would break the user out of keyword mode.  This will only be
      // triggered in regular (non-reorder) mode; in reorder mode,
      // navigation matches are marked as not allowed to be the default
      // match and hence IsTopMatchNavigation() will always return false.
      DCHECK(!omnibox_will_reorder_for_legal_default_match);
      DemoteKeywordNavigationMatchesPastTopQuery();
      ConvertResultsToAutocompleteMatches();
      DCHECK(!IsTopMatchNavigationInKeywordMode(
          omnibox_will_reorder_for_legal_default_match));
    }
    if (!HasKeywordDefaultMatchInKeywordMode()) {
      // In keyword mode, disregard the keyword verbatim suggested relevance
      // if necessary so there at least one keyword match that's allowed to
      // be the default match.
      keyword_results_.verbatim_relevance = -1;
      ConvertResultsToAutocompleteMatches();
    }
    if (IsTopMatchScoreTooLow(omnibox_will_reorder_for_legal_default_match)) {
      // Disregard the suggested verbatim relevance if the top score is below
      // the usual verbatim value. For example, a BarProvider may rely on
      // SearchProvider's verbatim or inlineable matches for input "foo" (all
      // allowed to be default match) to always outrank its own lowly-ranked
      // "bar" matches that shouldn't be the default match.
      default_results_.verbatim_relevance = -1;
      keyword_results_.verbatim_relevance = -1;
      ConvertResultsToAutocompleteMatches();
    }
    if (IsTopMatchSearchWithURLInput(
        omnibox_will_reorder_for_legal_default_match)) {
      // Disregard the suggested search and verbatim relevances if the input
      // type is URL and the top match is a highly-ranked search suggestion.
      // For example, prevent a search for "foo.com" from outranking another
      // provider's navigation for "foo.com" or "foo.com/url_from_history".
      ApplyCalculatedSuggestRelevance(&keyword_results_.suggest_results);
      ApplyCalculatedSuggestRelevance(&default_results_.suggest_results);
      default_results_.verbatim_relevance = -1;
      keyword_results_.verbatim_relevance = -1;
      ConvertResultsToAutocompleteMatches();
    }
    if (!HasValidDefaultMatch(omnibox_will_reorder_for_legal_default_match)) {
      // If the omnibox is not going to reorder results to put a legal default
      // match at the top, then this provider needs to guarantee that its top
      // scoring result is a legal default match (i.e., it's either a verbatim
      // match or inlinable).  For example, input "foo" should not invoke a
      // search for "bar", which would happen if the "bar" search match
      // outranked all other matches.  On the other hand, if the omnibox will
      // reorder matches as necessary to put a legal default match at the top,
      // all we need to guarantee is that SearchProvider returns a legal
      // default match.  (The omnibox always needs at least one legal default
      // match, and it relies on SearchProvider to always return one.)
      ApplyCalculatedRelevance();
      ConvertResultsToAutocompleteMatches();
    }
    DCHECK(!IsTopMatchNavigationInKeywordMode(
        omnibox_will_reorder_for_legal_default_match));
    DCHECK(HasKeywordDefaultMatchInKeywordMode());
    DCHECK(!IsTopMatchScoreTooLow(
        omnibox_will_reorder_for_legal_default_match));
    DCHECK(!IsTopMatchSearchWithURLInput(
        omnibox_will_reorder_for_legal_default_match));
    DCHECK(HasValidDefaultMatch(omnibox_will_reorder_for_legal_default_match));
  }

  const TemplateURL* keyword_url = providers_.GetKeywordProviderURL();
  if ((keyword_url != NULL) && HasKeywordDefaultMatchInKeywordMode()) {
    // If there is a keyword match that is allowed to be the default match,
    // then prohibit default provider matches from being the default match lest
    // such matches cause the user to break out of keyword mode.
    for (ACMatches::iterator it = matches_.begin(); it != matches_.end();
         ++it) {
      if (it->keyword != keyword_url->keyword())
        it->allowed_to_be_default_match = false;
    }
  }

  base::TimeTicks update_starred_start_time(base::TimeTicks::Now());
  UpdateStarredStateOfMatches();
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.UpdateStarredTime",
                      base::TimeTicks::Now() - update_starred_start_time);
  UpdateDone();
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.UpdateMatchesTime",
                      base::TimeTicks::Now() - update_matches_start_time);
}

void SearchProvider::AddNavigationResultsToMatches(
    const NavigationResults& navigation_results,
    ACMatches* matches) {
  for (NavigationResults::const_iterator it = navigation_results.begin();
        it != navigation_results.end(); ++it) {
    matches->push_back(NavigationToMatch(*it));
    // In the absence of suggested relevance scores, use only the single
    // highest-scoring result.  (The results are already sorted by relevance.)
    if (!it->relevance_from_server())
      return;
  }
}

void SearchProvider::AddHistoryResultsToMap(const HistoryResults& results,
                                            bool is_keyword,
                                            int did_not_accept_suggestion,
                                            MatchMap* map) {
  if (results.empty())
    return;

  base::TimeTicks start_time(base::TimeTicks::Now());
  bool prevent_inline_autocomplete = input_.prevent_inline_autocomplete() ||
      (input_.type() == AutocompleteInput::URL);
  const base::string16& input_text =
      is_keyword ? keyword_input_.text() : input_.text();
  bool input_multiple_words = HasMultipleWords(input_text);

  SuggestResults scored_results;
  if (!prevent_inline_autocomplete && input_multiple_words) {
    // ScoreHistoryResults() allows autocompletion of multi-word, 1-visit
    // queries if the input also has multiple words.  But if we were already
    // autocompleting a multi-word, multi-visit query, and the current input is
    // still a prefix of it, then changing the autocompletion suddenly feels
    // wrong.  To detect this case, first score as if only one word has been
    // typed, then check for a best result that is an autocompleted, multi-word
    // query.  If we find one, then just keep that score set.
    scored_results = ScoreHistoryResults(results, prevent_inline_autocomplete,
                                         false, input_text, is_keyword);
    if ((scored_results.front().relevance() <
             AutocompleteResult::kLowestDefaultScore) ||
        !HasMultipleWords(scored_results.front().suggestion()))
      scored_results.clear();  // Didn't detect the case above, score normally.
  }
  if (scored_results.empty())
    scored_results = ScoreHistoryResults(results, prevent_inline_autocomplete,
                                         input_multiple_words, input_text,
                                         is_keyword);
  for (SuggestResults::const_iterator i(scored_results.begin());
       i != scored_results.end(); ++i) {
    AddMatchToMap(*i, input_text, std::string(),
                  did_not_accept_suggestion, map);
  }
  UMA_HISTOGRAM_TIMES("Omnibox.SearchProvider.AddHistoryResultsTime",
                      base::TimeTicks::Now() - start_time);
}

SearchProvider::SuggestResults SearchProvider::ScoreHistoryResults(
    const HistoryResults& results,
    bool base_prevent_inline_autocomplete,
    bool input_multiple_words,
    const base::string16& input_text,
    bool is_keyword) {
  AutocompleteClassifier* classifier =
      AutocompleteClassifierFactory::GetForProfile(profile_);
  SuggestResults scored_results;
  const bool prevent_search_history_inlining =
      OmniboxFieldTrial::SearchHistoryPreventInlining(
          input_.current_page_classification());
  for (HistoryResults::const_iterator i(results.begin()); i != results.end();
       ++i) {
    // Don't autocomplete multi-word queries that have only been seen once
    // unless the user has typed more than one word.
    bool prevent_inline_autocomplete = base_prevent_inline_autocomplete ||
        (!input_multiple_words && (i->visits < 2) && HasMultipleWords(i->term));

    // Don't autocomplete search terms that would normally be treated as URLs
    // when typed. For example, if the user searched for "google.com" and types
    // "goog", don't autocomplete to the search term "google.com". Otherwise,
    // the input will look like a URL but act like a search, which is confusing.
    // NOTE: We don't check this in the following cases:
    //  * When inline autocomplete is disabled, we won't be inline
    //    autocompleting this term, so we don't need to worry about confusion as
    //    much.  This also prevents calling Classify() again from inside the
    //    classifier (which will corrupt state and likely crash), since the
    //    classifier always disables inline autocomplete.
    //  * When the user has typed the whole term, the "what you typed" history
    //    match will outrank us for URL-like inputs anyway, so we need not do
    //    anything special.
    if (!prevent_inline_autocomplete && classifier && (i->term != input_text)) {
      AutocompleteMatch match;
      classifier->Classify(i->term, false, false, &match, NULL);
      prevent_inline_autocomplete =
          !AutocompleteMatch::IsSearchType(match.type);
    }

    int relevance = CalculateRelevanceForHistory(
        i->time, is_keyword, !prevent_inline_autocomplete,
        prevent_search_history_inlining);
    scored_results.push_back(SuggestResult(
        i->term, AutocompleteMatchType::SEARCH_HISTORY, i->term,
        base::string16(), std::string(), std::string(), is_keyword, relevance,
        false, false, input_text));
  }

  // History returns results sorted for us.  However, we may have docked some
  // results' scores, so things are no longer in order.  Do a stable sort to get
  // things back in order without otherwise disturbing results with equal
  // scores, then force the scores to be unique, so that the order in which
  // they're shown is deterministic.
  std::stable_sort(scored_results.begin(), scored_results.end(),
                   CompareScoredResults());
  int last_relevance = 0;
  for (SuggestResults::iterator i(scored_results.begin());
       i != scored_results.end(); ++i) {
    if ((i != scored_results.begin()) && (i->relevance() >= last_relevance))
      i->set_relevance(last_relevance - 1);
    last_relevance = i->relevance();
  }

  return scored_results;
}

void SearchProvider::AddSuggestResultsToMap(const SuggestResults& results,
                                            const std::string& metadata,
                                            MatchMap* map) {
  for (size_t i = 0; i < results.size(); ++i) {
    const bool is_keyword = results[i].from_keyword_provider();
    const base::string16& input = is_keyword ? keyword_input_.text()
                                             : input_.text();
    AddMatchToMap(results[i], input, metadata, i, map);
  }
}

int SearchProvider::GetVerbatimRelevance(bool* relevance_from_server) const {
  // Use the suggested verbatim relevance score if it is non-negative (valid),
  // if inline autocomplete isn't prevented (always show verbatim on backspace),
  // and if it won't suppress verbatim, leaving no default provider matches.
  // Otherwise, if the default provider returned no matches and was still able
  // to suppress verbatim, the user would have no search/nav matches and may be
  // left unable to search using their default provider from the omnibox.
  // Check for results on each verbatim calculation, as results from older
  // queries (on previous input) may be trimmed for failing to inline new input.
  bool use_server_relevance =
      (default_results_.verbatim_relevance >= 0) &&
      !input_.prevent_inline_autocomplete() &&
      ((default_results_.verbatim_relevance > 0) ||
       !default_results_.suggest_results.empty() ||
       !default_results_.navigation_results.empty());
  if (relevance_from_server)
    *relevance_from_server = use_server_relevance;
  return use_server_relevance ?
      default_results_.verbatim_relevance : CalculateRelevanceForVerbatim();
}

int SearchProvider::CalculateRelevanceForVerbatim() const {
  if (!providers_.keyword_provider().empty())
    return 250;
  return CalculateRelevanceForVerbatimIgnoringKeywordModeState();
}

int SearchProvider::
    CalculateRelevanceForVerbatimIgnoringKeywordModeState() const {
  switch (input_.type()) {
    case AutocompleteInput::UNKNOWN:
    case AutocompleteInput::QUERY:
    case AutocompleteInput::FORCED_QUERY:
      return kNonURLVerbatimRelevance;

    case AutocompleteInput::URL:
      return 850;

    default:
      NOTREACHED();
      return 0;
  }
}

int SearchProvider::GetKeywordVerbatimRelevance(
    bool* relevance_from_server) const {
  // Use the suggested verbatim relevance score if it is non-negative (valid),
  // if inline autocomplete isn't prevented (always show verbatim on backspace),
  // and if it won't suppress verbatim, leaving no keyword provider matches.
  // Otherwise, if the keyword provider returned no matches and was still able
  // to suppress verbatim, the user would have no search/nav matches and may be
  // left unable to search using their keyword provider from the omnibox.
  // Check for results on each verbatim calculation, as results from older
  // queries (on previous input) may be trimmed for failing to inline new input.
  bool use_server_relevance =
      (keyword_results_.verbatim_relevance >= 0) &&
      !input_.prevent_inline_autocomplete() &&
      ((keyword_results_.verbatim_relevance > 0) ||
       !keyword_results_.suggest_results.empty() ||
       !keyword_results_.navigation_results.empty());
  if (relevance_from_server)
    *relevance_from_server = use_server_relevance;
  return use_server_relevance ?
      keyword_results_.verbatim_relevance :
      CalculateRelevanceForKeywordVerbatim(keyword_input_.type(),
                                           keyword_input_.prefer_keyword());
}

int SearchProvider::CalculateRelevanceForHistory(
    const base::Time& time,
    bool is_keyword,
    bool use_aggressive_method,
    bool prevent_search_history_inlining) const {
  // The relevance of past searches falls off over time. There are two distinct
  // equations used. If the first equation is used (searches to the primary
  // provider that we want to score aggressively), the score is in the range
  // 1300-1599 (unless |prevent_search_history_inlining|, in which case
  // it's in the range 1200-1299). If the second equation is used the
  // relevance of a search 15 minutes ago is discounted 50 points, while the
  // relevance of a search two weeks ago is discounted 450 points.
  double elapsed_time = std::max((base::Time::Now() - time).InSecondsF(), 0.0);
  bool is_primary_provider = is_keyword || !providers_.has_keyword_provider();
  if (is_primary_provider && use_aggressive_method) {
    // Searches with the past two days get a different curve.
    const double autocomplete_time = 2 * 24 * 60 * 60;
    if (elapsed_time < autocomplete_time) {
      int max_score = is_keyword ? 1599 : 1399;
      if (prevent_search_history_inlining)
        max_score = 1299;
      return max_score - static_cast<int>(99 *
          std::pow(elapsed_time / autocomplete_time, 2.5));
    }
    elapsed_time -= autocomplete_time;
  }

  const int score_discount =
      static_cast<int>(6.5 * std::pow(elapsed_time, 0.3));

  // Don't let scores go below 0.  Negative relevance scores are meaningful in
  // a different way.
  int base_score;
  if (is_primary_provider)
    base_score = (input_.type() == AutocompleteInput::URL) ? 750 : 1050;
  else
    base_score = 200;
  return std::max(0, base_score - score_discount);
}

void SearchProvider::AddMatchToMap(const SuggestResult& result,
                                   const base::string16& input_text,
                                   const std::string& metadata,
                                   int accepted_suggestion,
                                   MatchMap* map) {
  // On non-mobile, ask the instant controller for the appropriate start margin.
  // On mobile the start margin is unused, so leave the value as default there.
  int omnibox_start_margin = chrome::kDisableStartMargin;
#if !defined(OS_ANDROID) && !defined(IOS)
  if (chrome::IsInstantExtendedAPIEnabled()) {
    Browser* browser =
        chrome::FindBrowserWithProfile(profile_, chrome::GetActiveDesktop());
    if (browser && browser->instant_controller() &&
        browser->instant_controller()->instant()) {
      omnibox_start_margin =
          browser->instant_controller()->instant()->omnibox_bounds().x();
    }
  }
#endif  // !defined(OS_ANDROID) && !defined(IOS)

  const TemplateURL* template_url = result.from_keyword_provider() ?
      providers_.GetKeywordProviderURL() : providers_.GetDefaultProviderURL();
  AutocompleteMatch match = CreateSearchSuggestion(
      this, input_, input_text, result, template_url, accepted_suggestion,
      omnibox_start_margin,
      !result.from_keyword_provider() || providers_.default_provider().empty());
  if (!match.destination_url.is_valid())
    return;
  match.search_terms_args->bookmark_bar_pinned =
      profile_->GetPrefs()->GetBoolean(prefs::kShowBookmarkBar);
  match.RecordAdditionalInfo(kRelevanceFromServerKey,
                             result.relevance_from_server() ? kTrue : kFalse);
  match.RecordAdditionalInfo(kShouldPrefetchKey,
                             result.should_prefetch() ? kTrue : kFalse);

  if (!result.deletion_url().empty()) {
    GURL url(match.destination_url.GetOrigin().Resolve(result.deletion_url()));
    if (url.is_valid()) {
      match.RecordAdditionalInfo(kDeletionUrlKey, url.spec());
      match.deletable = true;
    }
  }

  // Metadata is needed only for prefetching queries.
  if (result.should_prefetch())
    match.RecordAdditionalInfo(kSuggestMetadataKey, metadata);

  // Try to add |match| to |map|.  If a match for |query_string| is already in
  // |map|, replace it if |match| is more relevant.
  // NOTE: Keep this ToLower() call in sync with url_database.cc.
  MatchKey match_key(
      std::make_pair(base::i18n::ToLower(result.suggestion()),
                     match.search_terms_args->suggest_query_params));
  const std::pair<MatchMap::iterator, bool> i(
      map->insert(std::make_pair(match_key, match)));

  bool should_prefetch = result.should_prefetch();
  if (!i.second) {
    // NOTE: We purposefully do a direct relevance comparison here instead of
    // using AutocompleteMatch::MoreRelevant(), so that we'll prefer "items
    // added first" rather than "items alphabetically first" when the scores are
    // equal. The only case this matters is when a user has results with the
    // same score that differ only by capitalization; because the history system
    // returns results sorted by recency, this means we'll pick the most
    // recent such result even if the precision of our relevance score is too
    // low to distinguish the two.
    if (match.relevance > i.first->second.relevance) {
      i.first->second = match;
    } else if (match.keyword == i.first->second.keyword) {
      // Old and new matches are from the same search provider. It is okay to
      // record one match's prefetch data onto a different match (for the same
      // query string) for the following reasons:
      // 1. Because the suggest server only sends down a query string from which
      // we construct a URL, rather than sending a full URL, and because we
      // construct URLs from query strings in the same way every time, the URLs
      // for the two matches will be the same. Therefore, we won't end up
      // prefetching something the server didn't intend.
      // 2. Presumably the server sets the prefetch bit on a match it things is
      // sufficiently relevant that the user is likely to choose it. Surely
      // setting the prefetch bit on a match of even higher relevance won't
      // violate this assumption.
      should_prefetch |= ShouldPrefetch(i.first->second);
      i.first->second.RecordAdditionalInfo(kShouldPrefetchKey,
                                           should_prefetch ? kTrue : kFalse);
      if (should_prefetch)
        i.first->second.RecordAdditionalInfo(kSuggestMetadataKey, metadata);
    }
  }
}

AutocompleteMatch SearchProvider::NavigationToMatch(
    const NavigationResult& navigation) {
  const base::string16& input = navigation.from_keyword_provider() ?
      keyword_input_.text() : input_.text();
  AutocompleteMatch match(this, navigation.relevance(), false,
                          AutocompleteMatchType::NAVSUGGEST);
  match.destination_url = navigation.url();

  // First look for the user's input inside the formatted url as it would be
  // without trimming the scheme, so we can find matches at the beginning of the
  // scheme.
  const URLPrefix* prefix =
      URLPrefix::BestURLPrefix(navigation.formatted_url(), input);
  size_t match_start = (prefix == NULL) ?
      navigation.formatted_url().find(input) : prefix->prefix.length();
  bool trim_http = !AutocompleteInput::HasHTTPScheme(input) &&
      (!prefix || (match_start != 0));
  const net::FormatUrlTypes format_types =
      net::kFormatUrlOmitAll & ~(trim_http ? 0 : net::kFormatUrlOmitHTTP);

  const std::string languages(
      profile_->GetPrefs()->GetString(prefs::kAcceptLanguages));
  size_t inline_autocomplete_offset = (prefix == NULL) ?
      base::string16::npos : (match_start + input.length());
  match.fill_into_edit +=
      AutocompleteInput::FormattedStringWithEquivalentMeaning(navigation.url(),
          net::FormatUrl(navigation.url(), languages, format_types,
                         net::UnescapeRule::SPACES, NULL, NULL,
                         &inline_autocomplete_offset));
  // Preserve the forced query '?' prefix in |match.fill_into_edit|.
  // Otherwise, user edits to a suggestion would show non-Search results.
  if (input_.type() == AutocompleteInput::FORCED_QUERY) {
    match.fill_into_edit.insert(0, base::ASCIIToUTF16("?"));
    if (inline_autocomplete_offset != base::string16::npos)
      ++inline_autocomplete_offset;
  }
  if (!input_.prevent_inline_autocomplete() &&
      (inline_autocomplete_offset != base::string16::npos)) {
    DCHECK(inline_autocomplete_offset <= match.fill_into_edit.length());
    // A navsuggestion can only be the default match when there is no
    // keyword provider active, lest it appear first and break the user
    // out of keyword mode.
    match.allowed_to_be_default_match =
        (providers_.GetKeywordProviderURL() == NULL);
    match.inline_autocompletion =
        match.fill_into_edit.substr(inline_autocomplete_offset);
  }

  match.contents = navigation.match_contents();
  match.contents_class = navigation.match_contents_class();
  match.description = navigation.description();
  AutocompleteMatch::ClassifyMatchInString(input, match.description,
      ACMatchClassification::NONE, &match.description_class);

  match.RecordAdditionalInfo(
      kRelevanceFromServerKey,
      navigation.relevance_from_server() ? kTrue : kFalse);
  match.RecordAdditionalInfo(kShouldPrefetchKey, kFalse);

  return match;
}

void SearchProvider::DemoteKeywordNavigationMatchesPastTopQuery() {
  // First, determine the maximum score of any keyword query match (verbatim or
  // query suggestion).
  bool relevance_from_server;
  int max_query_relevance = GetKeywordVerbatimRelevance(&relevance_from_server);
  if (!keyword_results_.suggest_results.empty()) {
    const SuggestResult& top_keyword = keyword_results_.suggest_results.front();
    const int suggest_relevance = top_keyword.relevance();
    if (suggest_relevance > max_query_relevance) {
      max_query_relevance = suggest_relevance;
      relevance_from_server = top_keyword.relevance_from_server();
    } else if (suggest_relevance == max_query_relevance) {
      relevance_from_server |= top_keyword.relevance_from_server();
    }
  }
  // If no query is supposed to appear, then navigational matches cannot
  // be demoted past it.  Get rid of suggested relevance scores for
  // navsuggestions and introduce the verbatim results again.  The keyword
  // verbatim match will outscore the navsuggest matches.
  if (max_query_relevance == 0) {
    ApplyCalculatedNavigationRelevance(&keyword_results_.navigation_results);
    ApplyCalculatedNavigationRelevance(&default_results_.navigation_results);
    keyword_results_.verbatim_relevance = -1;
    default_results_.verbatim_relevance = -1;
    return;
  }
  // Now we know we can enforce the minimum score constraint even after
  // the navigation matches are demoted.  Proceed to demote the navigation
  // matches to enforce the query-must-come-first constraint.
  // Cap the relevance score of all results.
  for (NavigationResults::iterator it =
           keyword_results_.navigation_results.begin();
       it != keyword_results_.navigation_results.end(); ++it) {
    if (it->relevance() < max_query_relevance)
      return;
    max_query_relevance = std::max(max_query_relevance - 1, 0);
    it->set_relevance(max_query_relevance);
    it->set_relevance_from_server(relevance_from_server);
  }
}

void SearchProvider::UpdateDone() {
  // We're done when the timer isn't running, there are no suggest queries
  // pending, and we're not waiting on Instant.
  done_ = !timer_.IsRunning() && (suggest_results_pending_ == 0);
}

bool SearchProvider::CanSendURL(
    const GURL& current_page_url,
    const GURL& suggest_url,
    const TemplateURL* template_url,
    AutocompleteInput::PageClassification page_classification,
    Profile* profile) {
  if (!current_page_url.is_valid())
    return false;

  // TODO(hfung): Show Most Visited on NTP with appropriate verbatim
  // description when the user actively focuses on the omnibox as discussed in
  // crbug/305366 if Most Visited (or something similar) will launch.
  if ((page_classification ==
       AutocompleteInput::INSTANT_NTP_WITH_FAKEBOX_AS_STARTING_FOCUS) ||
      (page_classification ==
       AutocompleteInput::INSTANT_NTP_WITH_OMNIBOX_AS_STARTING_FOCUS))
    return false;

  // Only allow HTTP URLs or HTTPS URLs for the same domain as the search
  // provider.
  if ((current_page_url.scheme() != content::kHttpScheme) &&
      ((current_page_url.scheme() != content::kHttpsScheme) ||
       !net::registry_controlled_domains::SameDomainOrHost(
           current_page_url, suggest_url,
           net::registry_controlled_domains::EXCLUDE_PRIVATE_REGISTRIES)))
    return false;

  // Make sure we are sending the suggest request through HTTPS to prevent
  // exposing the current page URL to networks before the search provider.
  if (!suggest_url.SchemeIs(content::kHttpsScheme))
    return false;

  // Don't run if there's no profile or in incognito mode.
  if (profile == NULL || profile->IsOffTheRecord())
    return false;

  // Don't run if we can't get preferences or search suggest is not enabled.
  PrefService* prefs = profile->GetPrefs();
  if (!prefs->GetBoolean(prefs::kSearchSuggestEnabled))
    return false;

  // Only make the request if we know that the provider supports zero suggest
  // (currently only the prepopulated Google provider).
  if (template_url == NULL || !template_url->SupportsReplacement() ||
      TemplateURLPrepopulateData::GetEngineType(*template_url) !=
      SEARCH_ENGINE_GOOGLE)
    return false;

  // Check field trials and settings allow sending the URL on suggest requests.
  ProfileSyncService* service =
      ProfileSyncServiceFactory::GetInstance()->GetForProfile(profile);
  browser_sync::SyncPrefs sync_prefs(prefs);
  if (!OmniboxFieldTrial::InZeroSuggestFieldTrial() ||
      service == NULL ||
      !service->IsSyncEnabledAndLoggedIn() ||
      !sync_prefs.GetPreferredDataTypes(syncer::UserTypes()).Has(
          syncer::PROXY_TABS) ||
      service->GetEncryptedDataTypes().Has(syncer::SESSIONS))
    return false;

  return true;
}
